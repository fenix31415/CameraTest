extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
#ifndef DEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= Version::PROJECT;
	*path += ".log"sv;
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef DEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_skse->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if (ver < SKSE::RUNTIME_1_5_39) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}

const float PI = 3.141592653589793f;
const float PI2 = PI * 2.0f;

float fix_polar(float a)
{
	while (a > PI2) a -= PI2;
	while (a < 0) a += PI2;
	return a;
}

float get_difference(float a, float b)
{
	float angle = abs(a - b);
	return angle > PI ? PI2 - angle : angle;
}

float move_polar_angle(float current, float target, float delta)
{
	if (get_difference(current, target) < delta) {
		return target;
	} else {
		bool plus = target >= current ? (target - current <= PI) : (current - target > PI);
		return fix_polar(plus ? (current + delta) : (current - delta));
	}
}

class SmoothLookingHook
{
	static inline float targetPitch = 0;
	static inline float currentPitch = 0;

	static void FromEulerAnglesXYZ(RE::NiQuaternion& q, float X, float Y, float Z)
	{
		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera->IsInThirdPerson())
			camera->ForceThirdPerson();

		if (camera->cameraTarget != RE::PlayerCharacter::GetSingleton()->GetHandle()) {
			targetPitch = X;

			if (targetPitch != currentPitch) {
				float dPitch = 0.1f * RE::GetSecondsSinceLastFrame();
				if (currentPitch - targetPitch <= 1.5707964f) {
					if (targetPitch - currentPitch > 1.5707964f)
						targetPitch = targetPitch - 6.2831855f;
				} else {
					targetPitch = targetPitch + 6.2831855f;
				}

				if (fabs(dPitch) <= fabs(targetPitch - currentPitch)) {
					if (targetPitch >= currentPitch)
						currentPitch = currentPitch + dPitch;
					else
						currentPitch = currentPitch - dPitch;
				} else {
					currentPitch = targetPitch;
				}

				currentPitch = fix_polar(currentPitch);
			}

			_FromEulerAnglesXYZ(q, currentPitch, Y, Z);
		} else {
			_FromEulerAnglesXYZ(q, X, Y, Z);
		}
	}

	static inline REL::Relocation<decltype(FromEulerAnglesXYZ)> _FromEulerAnglesXYZ;

public:
	static void Hook()
	{
		_FromEulerAnglesXYZ = SKSE::GetTrampoline().write_call<5>(REL::ID(49976).address() + 0xc5, FromEulerAnglesXYZ);
	}
};

void setGameSetting(int id, float val) { REL::safe_write(REL::ID(id).address(), &val, 4); }
float readGamesetting(int id) { return *REL::Relocation<float*>(REL::ID(id)); }
float changeGameSetting(int id, float val)
{
	float ans = readGamesetting(id);
	setGameSetting(id, val);
	return ans;
}

auto disableControls() {
	auto map = RE::ControlMap::GetSingleton();
	auto ans = map->enabledControls;
	using Con = RE::ControlMap::UEFlag;
	map->ToggleControls(Con::kFighting, false);
	map->ToggleControls(Con::kJumping, false);
	map->ToggleControls(Con::kMenu, false);
	map->ToggleControls(Con::kMovement, false);
	map->ToggleControls(Con::kMainFour, false);
	return ans;
}

class ShopCamera
{
	auto getState()
	{
		return (RE::ThirdPersonState*)RE::PlayerCamera::GetSingleton()
		    ->cameraStates[static_cast<size_t>(RE::CameraState::kThirdPerson)]
		    .get();
	}

	bool was1st = false;
	float wasfPitchZoomOutMaxDistCamera = 0;
	float wasfAutoVanityModeDelayCamera = 0;
	float wasfChaseCameraSpeedCamera = 0;
	RE::stl::enumeration<RE::ControlMap::UEFlag, std::uint32_t> wasenabledControls;

public:
	bool active = false;


	RE::NiPoint3 pos, angles;
	float curPitch = 0;
	float yawSpeed = 1.0f, pitchSpeed = 0.1f, movSpeed = 3.0f;

	void UpdateTargetPos(const RE::NiPoint3& a_dir)
	{
		FenixUtils::Geom::CombatUtilities__GetAimAnglesFromVector(a_dir, angles.z, angles.x);

		auto state = getState();
		state->posOffsetExpected = pos;
		angles.z = fix_polar(angles.z + RE::PlayerCharacter::GetSingleton()->GetHeading(false));
		state->targetYaw = angles.z;
	}

	void SetTargetPos(const RE::NiPoint3& a_pos, const RE::NiPoint3& a_dir)
	{
		if (active) {
			pos = a_pos;
			UpdateTargetPos(a_dir);
		}
	}

	void Start()
	{
		if (!active) {
			wasfPitchZoomOutMaxDistCamera = changeGameSetting(509905, 0);
			wasfAutoVanityModeDelayCamera = changeGameSetting(509848, 1000000.0f);
			wasfChaseCameraSpeedCamera = changeGameSetting(509911, yawSpeed);
			wasenabledControls = disableControls();

			auto state = getState();
			pos = state->posOffsetActual;

			auto camera = RE::PlayerCamera::GetSingleton();
			was1st = !camera->IsInThirdPerson();
			if (was1st)
				camera->ForceThirdPerson();

			RE::NiMatrix3 m;
			_generic_foo_<15612, void(RE::NiQuaternion&, RE::NiMatrix3&)>::eval(state->rotation, m);
			FenixUtils::Geom::CombatUtilities__GetAimAnglesFromVector(m * RE::NiPoint3(0, 1, 0), angles.z, angles.x);
			angles.y = 0;
			angles.z = fix_polar(angles.z);
			curPitch = angles.x;

			active = true;
		}
	}

	void End()
	{
		if (active) {
			auto camera = RE::PlayerCamera::GetSingleton();
			if (was1st) {
				camera->ForceFirstPerson();
			}
			setGameSetting(509905, wasfPitchZoomOutMaxDistCamera);
			setGameSetting(509848, wasfAutoVanityModeDelayCamera);
			setGameSetting(509911, wasfChaseCameraSpeedCamera);
			RE::ControlMap::GetSingleton()->enabledControls = wasenabledControls;
			active = false;
		}
	}
} shopCamera;

class FollowCamera
{
	void pick_actor(RE::Actor* a)
	{
		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera->IsInThirdPerson())
			camera->ForceThirdPerson();
		camera->cameraTarget = a->GetHandle();
	}

	auto getState()
	{
		return (RE::ThirdPersonState*)RE::PlayerCamera::GetSingleton()
		    ->cameraStates[static_cast<size_t>(RE::CameraState::kThirdPerson)]
		    .get();
	}

	bool wasfree = false;
	RE::stl::enumeration<RE::ControlMap::UEFlag, std::uint32_t> wasenabledControls;
	RE::NiPoint3 waspos;

public:
	bool active = false;

	void Begin(RE::Actor* a)
	{
		if (!active && a) {
			waspos = RE::PlayerCharacter::GetSingleton()->GetPosition();
			pick_actor(a);
			wasfree = getState()->freeRotationEnabled;
			wasenabledControls = disableControls();
			getState()->freeRotationEnabled = true;

			active = true;
		}
	}

	void End()
	{
		if (active) {
			pick_actor(RE::PlayerCharacter::GetSingleton());
			getState()->freeRotationEnabled = wasfree;
			RE::ControlMap::GetSingleton()->enabledControls = wasenabledControls;
			RE::PlayerCharacter::GetSingleton()->SetPosition(waspos, true);
			active = false;
		}
	}
} followCamera;

class Hooks
{
	static void UpdateRotation(RE::ThirdPersonState* _this)
	{
		if (!shopCamera.active) {
			return _UpdateRotation(_this);
		}

		_generic_foo_<69466, void(RE::NiQuaternion&, float, float, float)>::eval(_this->rotation, shopCamera.curPitch, 0,
			_this->currentYaw);
	}

	static void UpdateOffsets(RE::ThirdPersonState* _this)
	{
		if (!shopCamera.active) {
			return _UpdateOffsets(_this);
		}

		if (_this->posOffsetExpected != _this->posOffsetActual) {
			auto V = _this->posOffsetExpected - _this->posOffsetActual;
			auto dV = V * (shopCamera.movSpeed * RE::GetSecondsSinceLastFrame());
			if (dV.SqrLength() <= V.SqrLength()) {
				_this->posOffsetActual += dV;
			} else {
				_this->posOffsetActual = _this->posOffsetExpected;
			}
		}

		_this->currentYaw =
			move_polar_angle(_this->currentYaw, _this->targetYaw, shopCamera.yawSpeed * RE::GetSecondsSinceLastFrame());
		shopCamera.curPitch =
			move_polar_angle(shopCamera.curPitch, shopCamera.angles.x, shopCamera.pitchSpeed * RE::GetSecondsSinceLastFrame());
	}

	static void SetFreeRotationMode(RE::ThirdPersonState* _this, bool mode)
	{
		if (!followCamera.active)
			_SetFreeRotationMode(_this, mode);
	}

	static void Update(RE::PlayerCharacter* a, float delta)
	{
		_Update(a, delta);

		if (followCamera.active) {
			auto camera = RE::PlayerCamera::GetSingleton();
			if (auto target = camera->cameraTarget.get().get()) {
				a->SetPosition(target->GetPosition() + RE::NiPoint3{ 0, 0, 500 }, true);
			}
		}
	}

	static inline REL::Relocation<decltype(Update)> _Update;
	static inline REL::Relocation<decltype(SetFreeRotationMode)> _SetFreeRotationMode;
	static inline REL::Relocation<decltype(UpdateRotation)> _UpdateRotation;
	static inline REL::Relocation<decltype(UpdateOffsets)> _UpdateOffsets;

public:
	static void Hook()
	{
		_Update = REL::Relocation<uintptr_t>(REL::ID(261916)).write_vfunc(0xad, Update);
		_UpdateRotation = REL::Relocation<uintptr_t>(REL::ID(256647)).write_vfunc(14, UpdateRotation);
		_SetFreeRotationMode = REL::Relocation<uintptr_t>(REL::ID(256647)).write_vfunc(13, SetFreeRotationMode);
		_UpdateOffsets = SKSE::GetTrampoline().write_call<5>(REL::ID(49960).address() + 0xb0, UpdateOffsets);
	}
};

void start_shop() {
	shopCamera.Start();
	shopCamera.UpdateTargetPos({ 0, 1, 0 });
}

void end_shop() { shopCamera.End(); }

void set_yawSpeed(float val)
{
	shopCamera.yawSpeed = val;
	setGameSetting(509911, shopCamera.yawSpeed);
}

void set_pitchSpeed(float val) { shopCamera.pitchSpeed = val; }
void set_moveSpeed(float val) { shopCamera.movSpeed = val; }
void set_pos(const RE::NiPoint3& pos, const RE::NiPoint3& dir)
{
	shopCamera.pos = pos;
	shopCamera.UpdateTargetPos(dir);
}

void start_follow(RE::Actor* a) { followCamera.Begin(a); }
void end_follow() { followCamera.End(); }

void API_Shop_Start(RE::StaticFunctionTag*) { start_shop(); }
void API_Shop_End(RE::StaticFunctionTag*) { end_shop(); }
void API_Shop_SetYawSpeed(RE::StaticFunctionTag*, float val) { set_yawSpeed(val); }
void API_Shop_SetPitchSpeed(RE::StaticFunctionTag*, float val) { set_pitchSpeed(val); }
void API_Shop_SetMoveSpeed(RE::StaticFunctionTag*, float val) { set_moveSpeed(val); }
void API_Shop_SetPos(RE::StaticFunctionTag*, float posX, float posY, float posZ, float dirX, float dirY, float dirZ)
{
	set_pos({ posX, posY, posZ }, { dirX, dirY, dirZ });
}

void API_Follow_Start(RE::StaticFunctionTag*, RE::Actor* a) { start_follow(a); }
void API_Follow_End(RE::StaticFunctionTag*) { end_follow(); }


/*
namespace Gui
{
	void show_actor(RE::Actor*) { ImGui::Text("Actor found"); }

	void show()
	{
		static uint32_t actor_ID = 0;

		ImGui::Begin("Camera");

		ImGui::InputScalar("FormID", ImGuiDataType_U32, &actor_ID, NULL, NULL, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::SameLine();
		if (ImGui::Button("Consle")) {
			auto refr = RE::Console::GetSelectedRef().get();
			actor_ID = refr ? refr->formID : 0;
		}

		if (actor_ID > 0) {
			if (auto a = RE::TESForm::LookupByID<RE::Actor>(actor_ID)) {
				show_actor(a);
			}
		}

		ImGui::PushItemWidth(200);
		if (ImGui::Button("Start shop")) {
			start_shop();
		}

		if (ImGui::InputFloat("yawSpeed", &shopCamera.yawSpeed)) {
			set_yawSpeed(shopCamera.yawSpeed);
		}
		ImGui::SameLine();
		ImGui::InputFloat("pitchSpeed", &shopCamera.pitchSpeed);
		ImGui::SameLine();
		ImGui::InputFloat("SpeedMove", &shopCamera.movSpeed);

		static RE::NiPoint3 dir = { 0, 1, 0 };
		if (ImGui::Button("Set target pos")) {
			set_pos(shopCamera.pos, dir);
		}
		ImGui::InputFloat3("Pos", (float*)&shopCamera.pos);
		ImGui::InputFloat3("Dir", (float*)&dir);

		if (ImGui::Button("Armor")) {
			dir = { 0, -10, -2 };
			set_pos({ 0, -20, -20 }, dir);
		}
		if (ImGui::Button("Helmet")) {
			dir = { 20, -20, -3 };
			set_pos({ -20, 20, 10 }, dir);
		}
		if (ImGui::Button("All")) {
			dir = { 0, -10, -1 };
			set_pos({ 0, -300, 0 }, dir);
		}

		if (ImGui::Button("End shop")) {
			end_shop();
		}

		if (ImGui::Button("Pick")) {
			start_follow(RE::TESForm::LookupByID<RE::Actor>(actor_ID));
		}

		if (ImGui::Button("Back")) {
			end_follow();
		}
		ImGui::PopItemWidth();

		ImGui::End();
	}
}

bool enable = false;
bool hidden = true;

void flip_enable()
{
	enable = !enable;
	ImGui::GetIO().MouseDrawCursor = enable && !hidden;
}

void flip_hidden()
{
	hidden = !hidden;
	ImGui::GetIO().MouseDrawCursor = enable && !hidden;
}

const uint32_t enable_hotkey = 199;  // home
const uint32_t hide_hotkey = 207;    // end

void Process(const RE::ButtonEvent* button)
{
	if (button->IsPressed() && button->IsDown()) {
		if (button->GetIDCode() == enable_hotkey) {
			flip_enable();
		}
		if (button->GetIDCode() == hide_hotkey) {
			flip_hidden();
		}
	}
}

void show()
{
	if (!hidden) {
		ImGui::ShowDemoWindow();
		Gui::show();
	}
}

bool skipevents() { return enable && !hidden; }

using ImGuiHook = ImguiUtils::ImGuiHooks<Process, show, skipevents>;
*/

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		//SmoothLookingHook::Hook();
		Hooks::Hook();

		//ImGuiHook::Initialize();

		break;
	}
}

bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
{
	a_vm->RegisterFunction("API_Shop_Start", "Game", API_Shop_Start);
	a_vm->RegisterFunction("API_Shop_End", "Game", API_Shop_End);
	a_vm->RegisterFunction("API_Shop_SetYawSpeed", "Game", API_Shop_SetYawSpeed);
	a_vm->RegisterFunction("API_Shop_SetPitchSpeed", "Game", API_Shop_SetPitchSpeed);
	a_vm->RegisterFunction("API_Shop_SetMoveSpeed", "Game", API_Shop_SetMoveSpeed);
	a_vm->RegisterFunction("API_Shop_SetPos", "Game", API_Shop_SetPos);
	a_vm->RegisterFunction("API_Follow_Start", "Game", API_Follow_Start);
	a_vm->RegisterFunction("API_Follow_End", "Game", API_Follow_End);

	return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	auto g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
	if (!g_messaging) {
		logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
		return false;
	}

	logger::info("loaded");

	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 10);

	g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

	auto papyrus = SKSE::GetPapyrusInterface();
	if (!papyrus->Register(RegisterFuncs)) {
		return false;
	}

	return true;
}
