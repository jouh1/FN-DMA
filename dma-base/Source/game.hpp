#include <Pch.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"
#include <util.hpp>
#include <settings.hpp>
#include <render.hpp>
#include <offsets.hpp>

struct PlayerData {
    uintptr_t player_state;
    int team_id;
    uintptr_t pawn_private;
    uintptr_t root_comp;
    uintptr_t mesh;
    uintptr_t bone_array;
    uintptr_t bone_array_cache;
    BYTE isbot;
    bool fully_initialized = false;
};

struct EntityData {
    uintptr_t player_state;
    uintptr_t mesh;
    uintptr_t player_root;
    uintptr_t bone_array;
    uintptr_t pawn_private;
    uintptr_t bone_array_cache;
};

std::vector<EntityData> entities;
std::vector<EntityData> temp_entities;
std::mutex entities_mutex;
uintptr_t working_uworld;
uintptr_t text_selection;


void local_pointers() {
    while (true)
    {
        uintptr_t text_selection;
        for (auto i = 0; i < 255; i++) {
        	if (mem.Read<__int32>(cache::base + (i * 0x1000)) == 0x905A4D) {
        		text_selection = cache::base + ((i + 1) * 0x1000);
        	}
        }

        cache::uworld = mem.Read<uintptr_t>(text_selection + offsets::UWORLD);
        cache::game_instance = mem.Read<uintptr_t>(cache::uworld + offsets::GAME_INSTANCE);
        cache::game_state = mem.Read<uintptr_t>(cache::uworld + offsets::GAME_STATE);
        cache::local_players = mem.Read<uintptr_t>(mem.Read<uintptr_t>(cache::game_instance + offsets::LOCAL_PLAYERS));
        cache::player_controller = mem.Read<uintptr_t>(cache::local_players + offsets::PLAYER_CONTROLLER);
        cache::local_pawn = mem.Read<uintptr_t>(cache::player_controller + offsets::LOCAL_PAWN);
        cache::player_array = mem.Read<uintptr_t>(cache::game_state + offsets::PLAYER_ARRAY);
        
        if (cache::local_pawn) {
            cache::player_state = mem.Read<uintptr_t>(cache::local_pawn + offsets::PLAYER_STATE);
            cache::my_team_id = mem.Read<int>(cache::player_state + offsets::TEAM_INDEX);
            cache::root_component = mem.Read<uintptr_t>(cache::local_pawn + offsets::ROOT_COMPONENT);
        }
        
        cache::location_pointer = mem.Read<uintptr_t>(cache::uworld + 0x110);
        cache::rotation_pointer = mem.Read<uintptr_t>(cache::uworld + 0x120);

        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    }
}


void EntityLoop()
{
    std::vector<PlayerData> temp_player_data(200);
    auto handle = mem.CreateScatterHandle();

    while (true)
    {
        if (cache::game_state == 0 || cache::player_controller == 0 || cache::uworld == 0) {
            Sleep(200);
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        cache::player_count = mem.Read<int>(cache::game_state + (offsets::PLAYER_ARRAY + sizeof(uintptr_t)));

        if (cache::player_count <= 0 || cache::player_count > 200) return;

        temp_player_data.resize(cache::player_count);

        for (int i = 0; i < cache::player_count; i++) {
            mem.AddScatterReadRequest(handle, cache::player_array + i * sizeof(uintptr_t), &temp_player_data[i].player_state, sizeof(uintptr_t));
        }
        mem.ExecuteReadScatter(handle);

        for (int i = 0; i < cache::player_count; i++) {
            uintptr_t player_state = temp_player_data[i].player_state;
            if (player_state) {
                PlayerData& data = temp_player_data[i];
                mem.AddScatterReadRequest(handle, player_state + offsets::PAWN_PRIVATE, &data.pawn_private, sizeof(uintptr_t));
                mem.AddScatterReadRequest(handle, player_state + offsets::IS_BOT, &data.isbot, sizeof(BYTE));
            }
        }

        mem.ExecuteReadScatter(handle);

        for (int i = 0; i < cache::player_count; i++) {
            PlayerData& data = temp_player_data[i];
            if (data.pawn_private) {
                mem.AddScatterReadRequest(handle, data.pawn_private + offsets::MESH, &data.mesh, sizeof(uintptr_t));
                mem.AddScatterReadRequest(handle, data.pawn_private + offsets::ROOT_COMPONENT, &data.root_comp, sizeof(uintptr_t));
            }
        }

        mem.ExecuteReadScatter(handle);

        for (int i = 0; i < cache::player_count; i++) {
            PlayerData& data = temp_player_data[i];
            if (data.mesh) {
                mem.AddScatterReadRequest(handle, data.mesh + offsets::BONE_ARRAY, &data.bone_array, sizeof(uintptr_t));
                mem.AddScatterReadRequest(handle, data.mesh + offsets::BONE_ARRAY + 0x10, &data.bone_array_cache, sizeof(uintptr_t));
            }
        }

        mem.ExecuteReadScatter(handle);

        std::vector<EntityData> temp_entities;
        temp_entities.reserve(cache::player_count);

        for (int i = 0; i < cache::player_count; i++) {
            PlayerData& data = temp_player_data[i];

            if (data.pawn_private == cache::local_pawn) continue;

            EntityData entity;
            entity.mesh = data.mesh;
            entity.player_root = data.root_comp;
            entity.bone_array = data.bone_array;
            entity.pawn_private = data.pawn_private;
            entity.bone_array_cache = data.bone_array_cache;

            temp_entities.push_back(entity);
        }

        {
            std::lock_guard<std::mutex> lock(entities_mutex);
            entities = std::move(temp_entities);
        }

        temp_player_data.clear();
        temp_entities.clear();
    }
}

static constexpr size_t MAX_ENTITIES = 200;
alignas(16) FTransform headTransforms[MAX_ENTITIES];
alignas(16) FTransform rootTransforms[MAX_ENTITIES];
alignas(16) FTransform comp2Worlds[MAX_ENTITIES];
BYTE isDyingFlags[MAX_ENTITIES];
float lastSubmitTimes[MAX_ENTITIES];
float lastRenderTimes[MAX_ENTITIES];

void draw_entities() {

    float closest_screen_distance = FLT_MAX;
    float closest_meter_distance = FLT_MAX;
    uintptr_t closest_mesh = NULL;
    Vector3 closest_bone = { 0, 0 ,0 };

    if (entities.empty()) {
        return;
    }

    Camera view_point{};
    FNRot fnrot{};
    float fov;
    uintptr_t TargetedPawn;

    auto entityScatter = mem.CreateScatterHandle();

    mem.AddScatterReadRequest(entityScatter, cache::location_pointer + offsetof(Vector3, x), &view_point.location.x, sizeof(double));
    mem.AddScatterReadRequest(entityScatter, cache::location_pointer + offsetof(Vector3, y), &view_point.location.y, sizeof(double));
    mem.AddScatterReadRequest(entityScatter, cache::location_pointer + offsetof(Vector3, z), &view_point.location.z, sizeof(double));

    mem.AddScatterReadRequest(entityScatter, cache::rotation_pointer, &fnrot.a, sizeof(double));
    mem.AddScatterReadRequest(entityScatter, cache::rotation_pointer + 0x20, &fnrot.b, sizeof(double));
    mem.AddScatterReadRequest(entityScatter, cache::rotation_pointer + 0x1d0, &fnrot.c, sizeof(double));
    mem.AddScatterReadRequest(entityScatter, cache::player_controller + 0x394, &fov, sizeof(float));

    size_t entityCount = std::min(entities.size(), MAX_ENTITIES);

    for (size_t i = 0; i < entityCount; ++i) {
        const auto& entity = entities[i];

        uintptr_t bone_array = entity.bone_array;
        if (!bone_array) bone_array = entity.bone_array_cache;


        mem.AddScatterReadRequest(entityScatter, bone_array + (110 * 0x60), &headTransforms[i], sizeof(FTransform));
        mem.AddScatterReadRequest(entityScatter, bone_array + (0 * 0x60), &rootTransforms[i], sizeof(FTransform));
        mem.AddScatterReadRequest(entityScatter, entity.mesh + offsets::COMPONENT_TO_WORLD, &comp2Worlds[i], sizeof(FTransform));

        mem.AddScatterReadRequest(entityScatter, entity.pawn_private + offsets::IS_DYING, &isDyingFlags[i], sizeof(BYTE));
        mem.AddScatterReadRequest(entityScatter, entity.pawn_private + offsets::LAST_SUMBIT_TIME, &lastSubmitTimes[i], sizeof(BYTE));
        mem.AddScatterReadRequest(entityScatter, entity.pawn_private + offsets::LAST_SUMBIT_TIME_ON_SCREEN, &lastRenderTimes[i], sizeof(BYTE));

    }

    mem.ExecuteReadScatter(entityScatter);

    view_point.rotation.x = asin(fnrot.c) * (180.0 / M_PI);
    view_point.rotation.y = ((atan2(fnrot.a * -1, fnrot.b) * (180.0 / M_PI)) * -1) * -1;
    view_point.fov = fov * 90.f;

    cache::local_camera = view_point;

    for (size_t i = 0; i < entityCount; ++i) {

        if (isDyingFlags[i] >> 4) continue;

        Vector3 head3d = GetBoneFT(headTransforms[i], comp2Worlds[i]);
        Vector2 head2d = project_world_to_screen(head3d);
        Vector3 bottom3d = GetBoneFT(rootTransforms[i], comp2Worlds[i]);
        Vector2 bottom2d = project_world_to_screen(bottom3d);

        int box_height = abs(head2d.y - bottom2d.y);
        int box_width = static_cast<int>(box_height * 0.50f);
        float distance = bottom3d.distance(cache::local_camera.location) / 100.0f;

        bool is_vis = lastRenderTimes[i] + 0.06f >= lastSubmitTimes[i];

        ImColor box_color = is_vis 
            ? ImColor(settings::visuals::boxColor[0], settings::visuals::boxColor[1], settings::visuals::boxColor[2], settings::visuals::boxColor[3])
            : ImColor(settings::visuals::boxColor2[0], settings::visuals::boxColor2[1], settings::visuals::boxColor2[2], settings::visuals::boxColor2[3]);


        if (settings::visuals::box) {
            draw_cornered_box(head2d.x - (box_width / 2), head2d.y, box_width, box_height, box_color, 1);
        }
        if (settings::visuals::fill_box) {
            ImColor fill_color = box_color;
            fill_color.Value.w = 0.5f;
            draw_filled_rect(head2d.x - (box_width / 2), head2d.y, box_width, box_height, fill_color);
        }
        if (settings::visuals::line) {
            draw_line(bottom2d, box_color);
        }
        if (settings::visuals::distance) {
            draw_distance(head2d, distance, ImColor(250, 250, 250, 250));
        }
    }
}

// Render

WPARAM render_loop() {
    ZeroMemory(&messager, sizeof(MSG));
    while (messager.message != WM_QUIT) {
        if (PeekMessage(&messager, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&messager);
            DispatchMessage(&messager);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        POINT p;
        GetCursorPos(&p);
        io.MousePos.x = static_cast<float>(p.x);
        io.MousePos.y = static_cast<float>(p.y);
        io.MouseDown[0] = GetAsyncKeyState(VK_LBUTTON) != 0;
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_entities();
        render_menu();
        ImGui::EndFrame();
        p_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        p_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        p_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        p_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (p_device->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            p_device->EndScene();
        }
        HRESULT result = p_device->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST && p_device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            p_device->Reset(&p_params);
            ImGui_ImplDX9_CreateDeviceObjects();
        }
    }
    return messager.wParam;
}

bool init() {
    create_overlay();
    return SUCCEEDED(directx_init());
}
