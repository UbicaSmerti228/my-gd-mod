#include <windows.h>
#include <vector>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include <MinHook.h>

using json = nlohmann::json;

// Проксирование для невидимости (перенаправляем системные вызовы)
#pragma comment(linker, "/export:XInputGetState=C:\\Windows\\System32\\xinput9_1_0.XInputGetState")
#pragma comment(linker, "/export:XInputSetState=C:\\Windows\\System32\\xinput9_1_0.XInputSetState")

struct MacroInput {
    int frame;
    int btn;
    bool down;
    bool used = false;
};

std::vector<MacroInput> macroInputs;
int currentFrame = 0;
bool isAutoClicking = false;

// Оффсеты для версии 2.204 (актуальные сейчас)
uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
uintptr_t pushAddr = base + 0x1B69F0; 
uintptr_t updateAddr = base + 0x1C2F30;
uintptr_t resetAddr = base + 0x2EFC40;

typedef void(__thiscall* PushBtn)(void*, int, bool);
PushBtn oPush;

void __fastcall hkPush(void* self, void*, int btn, bool p2) {
    // Проверяем: если в макросе есть клик рядом (допустим, в пределах 10 кадров)
    for (auto& mi : macroInputs) {
        if (mi.down && !mi.used && abs(mi.frame - currentFrame) < 10) {
            // Если игрок нажал чуть раньше - блокируем и ждем кадра из макроса
            if (currentFrame < mi.frame) {
                isAutoClicking = true; 
                return; 
            }
        }
    }
    oPush(self, btn, p2);
}

typedef void(__thiscall* Update)(void*, float);
Update oUpdate;

void __fastcall hkUpdate(void* self, void*, float dt) {
    currentFrame++;
    
    // Доводка клика до идеала
    for (auto& mi : macroInputs) {
        if (currentFrame == mi.frame && isAutoClicking && mi.down) {
            oPush(self, mi.btn, false);
            isAutoClicking = false;
            mi.used = true;
        }
    }
    oUpdate(self, dt);
}

typedef void(__thiscall* Reset)(void*);
Reset oReset;
void __fastcall hkReset(void* self, void*) {
    currentFrame = 0;
    isAutoClicking = false;
    for (auto& mi : macroInputs) mi.used = false;
    oReset(self);
}

void LoadMacro() {
    std::ifstream f("macro.json");
    if (f.is_open()) {
        json j; f >> j;
        for (auto& item : j["inputs"]) {
            macroInputs.push_back({item["frame"], item["btn"], item["down"]});
        }
    }
}

DWORD WINAPI MainThread(LPVOID) {
    LoadMacro();
    MH_Initialize();
    MH_CreateHook((LPVOID)pushAddr, hkPush, (LPVOID*)&oPush);
    MH_CreateHook((LPVOID)updateAddr, hkUpdate, (LPVOID*)&oUpdate);
    MH_CreateHook((LPVOID)resetAddr, hkReset, (LPVOID*)&oReset);
    MH_EnableHook(MH_ALL_HOOKS);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) CreateThread(0, 0, MainThread, h, 0, 0);
    return TRUE;
}
