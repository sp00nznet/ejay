/* probe.c - can the 1999 engine and its graphics DLL still be driven directly?
 *
 * PXD32D4.DLL and PXD32CL1.DLL are native 32-bit code that imports only DLLs
 * Windows still ships. Nothing here is lifted: this loads them as they are and
 * asks which entry points resolve, which is the cheapest possible test of
 * whether eJay 2 can be driven at all before anything is recompiled.
 */
#include <stdio.h>
#include <windows.h>

static const char *ENGINE[] = {
    "AInit", "ADevice", "AStart", "AStop", "AEnd", "APlay", "ATimer",
    "AGetTime", "ALautSet", "ALautGet", "ABilder", "ABildpos", "AExport",
    "AWaveDauer", "ASortInit", "ASortIn", "ASortStart", "AGetFree", "AGetFull",
    "AFenster", "AMemory", "ASetPfad", "ASetPitch", "ALoad", "APos", 0
};
static const char *GFX[] = {
    "GFX_SampleInit", "GFX_SampleZeichne", "GFX_SampleZeichneSel",
    "GFX_SampleClose", "GFX_SetActiveWindow", "GFX_VolBarInit",
    "GFX_VolBarDraw", "GFX_IntroInitScreen", "GFX_IntroInitSplash",
    "GFX_IntroShowSplash", "GFX_IntroRefresh", "GFX_IntroClose",
    "GFX_AnimationPhase", 0
};

static int probe(const char *dll, const char **names)
{
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("  %-16s FAILED to load (%lu)\n", dll, GetLastError()); return 0; }
    printf("  %s loaded at %p\n", dll, (void *)h);
    int ok = 0, miss = 0;
    for (int i = 0; names[i]; i++) {
        if (GetProcAddress(h, names[i])) ok++;
        else { printf("      missing: %s\n", names[i]); miss++; }
    }
    printf("      %d resolved, %d missing\n", ok, miss);
    return 1;
}

int main(void)
{
    printf("Dance eJay 2 (1999) - driving the shipped DLLs directly\n\n");
    probe("PXD32D4.DLL", ENGINE);
    probe("PXD32CL1.DLL", GFX);
    probe("PXD98DB.DLL", (const char *[]){ "DoMixLoadDlg", "SetDlgFont", "VB_CallBack", 0 });
    return 0;
}
