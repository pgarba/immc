#include "theme.h"
#include <string.h>

static MCThemeColors g_colors;

#define C(col, r, g, b, a) s.Colors[col] = ImVec4(r, g, b, a)

static void accents_default(void) {
    g_colors.title_focused     = ImVec4(0.20f, 0.45f, 0.85f, 1.0f);
    g_colors.title_focused_hov = ImVec4(0.25f, 0.50f, 0.90f, 1.0f);
    g_colors.title_unfocused   = ImVec4(0.15f, 0.17f, 0.20f, 1.0f);
    g_colors.dir               = ImVec4(0.30f, 0.60f, 1.00f, 1.0f);
    g_colors.exec              = ImVec4(0.30f, 1.00f, 0.30f, 1.0f);
    g_colors.diff_missing      = ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
    g_colors.diff_differ       = ImVec4(1.00f, 0.78f, 0.30f, 1.0f);
    g_colors.marked_bg         = IM_COL32(51, 140, 64, 255);
    g_colors.cursor_bg         = IM_COL32(51, 115, 217, 255);
    g_colors.cursor_outline    = IM_COL32(140, 190, 255, 255);
    g_colors.window_bg         = ImVec4(0.06f, 0.09f, 0.15f, 1.0f);
}

// ---------------------------------------------------------------- dark
static void theme_dark(void) {
    ImGui::StyleColorsDark();
    accents_default();
}

// ---------------------------------------------------------------- light
static void theme_light(void) {
    ImGui::StyleColorsLight();
    accents_default();
    g_colors.title_unfocused = ImVec4(0.85f, 0.87f, 0.90f, 1.0f);
    g_colors.exec            = ImVec4(0.10f, 0.50f, 0.10f, 1.0f);
    g_colors.diff_missing    = ImVec4(0.70f, 0.10f, 0.10f, 1.0f);
    g_colors.diff_differ     = ImVec4(0.60f, 0.40f, 0.00f, 1.0f);
    g_colors.window_bg       = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
}

// ---------------------------------------------------------------- classic
static void theme_classic(void) {
    ImGui::StyleColorsClassic();
    accents_default();
    g_colors.window_bg = ImVec4(0.10f, 0.15f, 0.25f, 1.0f);
}

// ---------------------------------------------------------------- gruvbox dark
static void theme_gruvbox_dark(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    C(ImGuiCol_Text,                  0.922f, 0.859f, 0.698f, 1.00f); // ebdbb2
    C(ImGuiCol_TextDisabled,          0.573f, 0.514f, 0.455f, 1.00f); // 928374
    C(ImGuiCol_WindowBg,              0.157f, 0.157f, 0.157f, 1.00f); // 282828
    C(ImGuiCol_ChildBg,               0.196f, 0.188f, 0.184f, 1.00f); // 32302f
    C(ImGuiCol_PopupBg,               0.235f, 0.220f, 0.212f, 0.98f); // 3c3836
    C(ImGuiCol_Border,                0.314f, 0.286f, 0.271f, 0.70f); // 504945
    C(ImGuiCol_FrameBg,               0.235f, 0.220f, 0.212f, 1.00f);
    C(ImGuiCol_FrameBgHovered,        0.314f, 0.286f, 0.271f, 1.00f);
    C(ImGuiCol_FrameBgActive,         0.400f, 0.361f, 0.329f, 1.00f); // 665c54
    C(ImGuiCol_TitleBg,               0.114f, 0.125f, 0.129f, 1.00f); // 1d2021
    C(ImGuiCol_TitleBgActive,         0.157f, 0.157f, 0.157f, 1.00f);
    C(ImGuiCol_MenuBarBg,             0.114f, 0.125f, 0.129f, 1.00f);
    C(ImGuiCol_ScrollbarBg,           0.157f, 0.157f, 0.157f, 0.53f);
    C(ImGuiCol_ScrollbarGrab,         0.235f, 0.220f, 0.212f, 1.00f);
    C(ImGuiCol_ScrollbarGrabHovered,  0.314f, 0.286f, 0.271f, 1.00f);
    C(ImGuiCol_ScrollbarGrabActive,   0.400f, 0.361f, 0.329f, 1.00f);
    C(ImGuiCol_CheckMark,             0.843f, 0.600f, 0.129f, 1.00f); // d79921
    C(ImGuiCol_SliderGrab,            0.843f, 0.600f, 0.129f, 1.00f);
    C(ImGuiCol_SliderGrabActive,      0.980f, 0.741f, 0.184f, 1.00f); // fabd2f
    C(ImGuiCol_Button,                0.235f, 0.220f, 0.212f, 1.00f);
    C(ImGuiCol_ButtonHovered,         0.314f, 0.286f, 0.271f, 1.00f);
    C(ImGuiCol_ButtonActive,          0.400f, 0.361f, 0.329f, 1.00f);
    C(ImGuiCol_Header,                0.314f, 0.286f, 0.271f, 0.55f);
    C(ImGuiCol_HeaderHovered,         0.400f, 0.361f, 0.329f, 0.80f);
    C(ImGuiCol_HeaderActive,          0.502f, 0.463f, 0.424f, 0.80f); // 7c6f64
    C(ImGuiCol_Separator,             0.314f, 0.286f, 0.271f, 0.50f);
    C(ImGuiCol_SeparatorHovered,      0.843f, 0.600f, 0.129f, 0.78f);
    C(ImGuiCol_SeparatorActive,       0.843f, 0.600f, 0.129f, 1.00f);
    C(ImGuiCol_ResizeGrip,            0.314f, 0.286f, 0.271f, 0.20f);
    C(ImGuiCol_ResizeGripHovered,     0.843f, 0.600f, 0.129f, 0.67f);
    C(ImGuiCol_ResizeGripActive,      0.843f, 0.600f, 0.129f, 0.95f);
    C(ImGuiCol_Tab,                   0.235f, 0.220f, 0.212f, 1.00f);
    C(ImGuiCol_TabHovered,            0.843f, 0.600f, 0.129f, 0.80f);
    C(ImGuiCol_TabActive,             0.400f, 0.361f, 0.329f, 1.00f);
    C(ImGuiCol_TextSelectedBg,        0.271f, 0.522f, 0.533f, 0.59f); // 458588
    C(ImGuiCol_ModalWindowDimBg,      0.000f, 0.000f, 0.000f, 0.35f);

    g_colors.title_focused     = ImVec4(0.271f, 0.522f, 0.533f, 1.0f); // 458588
    g_colors.title_focused_hov = ImVec4(0.350f, 0.600f, 0.610f, 1.0f);
    g_colors.title_unfocused   = ImVec4(0.235f, 0.220f, 0.212f, 1.0f); // 3c3836
    g_colors.dir               = ImVec4(0.514f, 0.647f, 0.596f, 1.0f); // 83a598
    g_colors.exec              = ImVec4(0.722f, 0.733f, 0.149f, 1.0f); // b8bb26
    g_colors.diff_missing      = ImVec4(0.984f, 0.286f, 0.204f, 1.0f); // fb4933
    g_colors.diff_differ       = ImVec4(0.984f, 0.741f, 0.184f, 1.0f); // fabd2f
    g_colors.marked_bg         = IM_COL32(90, 102, 26, 255);           // 98971a dark
    g_colors.cursor_bg         = IM_COL32(69, 133, 136, 255);          // 458588
    g_colors.cursor_outline    = IM_COL32(131, 165, 152, 255);         // 83a598
    g_colors.window_bg         = ImVec4(0.157f, 0.157f, 0.157f, 1.0f);
}

// ---------------------------------------------------------------- gruvbox light
static void theme_gruvbox_light(void) {
    ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    C(ImGuiCol_Text,                  0.235f, 0.220f, 0.212f, 1.00f); // 3c3836
    C(ImGuiCol_TextDisabled,          0.706f, 0.651f, 0.569f, 1.00f); // bdae93
    C(ImGuiCol_WindowBg,              0.984f, 0.945f, 0.780f, 1.00f); // fbf1c7
    C(ImGuiCol_ChildBg,               0.922f, 0.859f, 0.698f, 1.00f); // ebdbb2
    C(ImGuiCol_PopupBg,               0.984f, 0.945f, 0.780f, 0.98f);
    C(ImGuiCol_Border,                0.835f, 0.769f, 0.631f, 0.70f); // d5c4a1
    C(ImGuiCol_FrameBg,               0.922f, 0.859f, 0.698f, 1.00f);
    C(ImGuiCol_FrameBgHovered,        0.870f, 0.796f, 0.624f, 1.00f);
    C(ImGuiCol_FrameBgActive,         0.835f, 0.769f, 0.631f, 1.00f);
    C(ImGuiCol_TitleBg,               0.922f, 0.859f, 0.698f, 1.00f);
    C(ImGuiCol_TitleBgActive,         0.984f, 0.945f, 0.780f, 1.00f);
    C(ImGuiCol_MenuBarBg,             0.922f, 0.859f, 0.698f, 1.00f);
    C(ImGuiCol_CheckMark,             0.706f, 0.459f, 0.078f, 1.00f); // b57614
    C(ImGuiCol_SliderGrab,            0.706f, 0.459f, 0.078f, 1.00f);
    C(ImGuiCol_SliderGrabActive,      0.835f, 0.545f, 0.078f, 1.00f);
    C(ImGuiCol_Button,                0.922f, 0.859f, 0.698f, 1.00f);
    C(ImGuiCol_ButtonHovered,         0.870f, 0.796f, 0.624f, 1.00f);
    C(ImGuiCol_ButtonActive,          0.835f, 0.769f, 0.631f, 1.00f);
    C(ImGuiCol_Header,                0.870f, 0.796f, 0.624f, 0.55f);
    C(ImGuiCol_HeaderHovered,         0.835f, 0.769f, 0.631f, 0.80f);
    C(ImGuiCol_HeaderActive,          0.769f, 0.690f, 0.549f, 0.80f);
    C(ImGuiCol_Separator,             0.835f, 0.769f, 0.631f, 0.50f);
    C(ImGuiCol_SeparatorHovered,      0.706f, 0.459f, 0.078f, 0.78f);
    C(ImGuiCol_SeparatorActive,       0.706f, 0.459f, 0.078f, 1.00f);
    C(ImGuiCol_Tab,                   0.922f, 0.859f, 0.698f, 1.00f);
    C(ImGuiCol_TabHovered,            0.706f, 0.459f, 0.078f, 0.80f);
    C(ImGuiCol_TabActive,             0.835f, 0.769f, 0.631f, 1.00f);
    C(ImGuiCol_TextSelectedBg,        0.027f, 0.400f, 0.471f, 0.35f); // 076678

    g_colors.title_focused     = ImVec4(0.250f, 0.450f, 0.500f, 1.0f);
    g_colors.title_focused_hov = ImVec4(0.300f, 0.520f, 0.580f, 1.0f);
    g_colors.title_unfocused   = ImVec4(0.922f, 0.859f, 0.698f, 1.0f); // ebdbb2
    g_colors.dir               = ImVec4(0.027f, 0.400f, 0.471f, 1.0f); // 076678
    g_colors.exec              = ImVec4(0.259f, 0.482f, 0.345f, 1.0f); // 427b58
    g_colors.diff_missing      = ImVec4(0.616f, 0.086f, 0.086f, 1.0f); // 9d0006
    g_colors.diff_differ       = ImVec4(0.710f, 0.373f, 0.000f, 1.0f); // af5f00
    g_colors.marked_bg         = IM_COL32(160, 191, 140, 255);
    g_colors.cursor_bg         = IM_COL32(115, 158, 148, 255);
    g_colors.cursor_outline    = IM_COL32(7, 102, 120, 255);           // 076678
    g_colors.window_bg         = ImVec4(0.984f, 0.945f, 0.780f, 1.0f);
}

// ---------------------------------------------------------------- solarized dark
static void theme_solarized_dark(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    C(ImGuiCol_Text,                  0.514f, 0.580f, 0.588f, 1.00f); // 839496
    C(ImGuiCol_TextDisabled,          0.345f, 0.431f, 0.459f, 1.00f); // 586e75
    C(ImGuiCol_WindowBg,              0.000f, 0.169f, 0.212f, 1.00f); // 002b36
    C(ImGuiCol_ChildBg,               0.027f, 0.212f, 0.259f, 1.00f); // 073642
    C(ImGuiCol_PopupBg,               0.027f, 0.212f, 0.259f, 0.98f);
    C(ImGuiCol_Border,                0.345f, 0.431f, 0.459f, 0.70f); // 586e75
    C(ImGuiCol_FrameBg,               0.027f, 0.212f, 0.259f, 1.00f);
    C(ImGuiCol_FrameBgHovered,        0.035f, 0.263f, 0.322f, 1.00f);
    C(ImGuiCol_FrameBgActive,         0.043f, 0.314f, 0.384f, 1.00f);
    C(ImGuiCol_TitleBg,               0.000f, 0.129f, 0.169f, 1.00f); // 00212b
    C(ImGuiCol_TitleBgActive,         0.000f, 0.169f, 0.212f, 1.00f);
    C(ImGuiCol_MenuBarBg,             0.000f, 0.129f, 0.169f, 1.00f);
    C(ImGuiCol_ScrollbarBg,           0.000f, 0.169f, 0.212f, 0.53f);
    C(ImGuiCol_ScrollbarGrab,         0.027f, 0.212f, 0.259f, 1.00f);
    C(ImGuiCol_ScrollbarGrabHovered,  0.035f, 0.263f, 0.322f, 1.00f);
    C(ImGuiCol_ScrollbarGrabActive,   0.043f, 0.314f, 0.384f, 1.00f);
    C(ImGuiCol_CheckMark,             0.149f, 0.545f, 0.824f, 1.00f); // 268bd2
    C(ImGuiCol_SliderGrab,            0.149f, 0.545f, 0.824f, 1.00f);
    C(ImGuiCol_SliderGrabActive,      0.200f, 0.630f, 0.870f, 1.00f);
    C(ImGuiCol_Button,                0.027f, 0.212f, 0.259f, 1.00f);
    C(ImGuiCol_ButtonHovered,         0.035f, 0.263f, 0.322f, 1.00f);
    C(ImGuiCol_ButtonActive,          0.043f, 0.314f, 0.384f, 1.00f);
    C(ImGuiCol_Header,                0.027f, 0.212f, 0.259f, 0.55f);
    C(ImGuiCol_HeaderHovered,         0.035f, 0.263f, 0.322f, 0.80f);
    C(ImGuiCol_HeaderActive,          0.043f, 0.314f, 0.384f, 0.80f);
    C(ImGuiCol_Separator,             0.345f, 0.431f, 0.459f, 0.50f);
    C(ImGuiCol_SeparatorHovered,      0.149f, 0.545f, 0.824f, 0.78f);
    C(ImGuiCol_SeparatorActive,       0.149f, 0.545f, 0.824f, 1.00f);
    C(ImGuiCol_ResizeGrip,            0.345f, 0.431f, 0.459f, 0.20f);
    C(ImGuiCol_ResizeGripHovered,     0.149f, 0.545f, 0.824f, 0.67f);
    C(ImGuiCol_ResizeGripActive,      0.149f, 0.545f, 0.824f, 0.95f);
    C(ImGuiCol_Tab,                   0.027f, 0.212f, 0.259f, 1.00f);
    C(ImGuiCol_TabHovered,            0.149f, 0.545f, 0.824f, 0.80f);
    C(ImGuiCol_TabActive,             0.043f, 0.314f, 0.384f, 1.00f);
    C(ImGuiCol_TextSelectedBg,        0.149f, 0.545f, 0.824f, 0.45f);
    C(ImGuiCol_ModalWindowDimBg,      0.000f, 0.000f, 0.000f, 0.35f);

    g_colors.title_focused     = ImVec4(0.090f, 0.350f, 0.520f, 1.0f);
    g_colors.title_focused_hov = ImVec4(0.130f, 0.450f, 0.650f, 1.0f);
    g_colors.title_unfocused   = ImVec4(0.027f, 0.212f, 0.259f, 1.0f); // 073642
    g_colors.dir               = ImVec4(0.149f, 0.545f, 0.824f, 1.0f); // 268bd2
    g_colors.exec              = ImVec4(0.522f, 0.600f, 0.000f, 1.0f); // 859900
    g_colors.diff_missing      = ImVec4(0.863f, 0.196f, 0.184f, 1.0f); // dc322f
    g_colors.diff_differ       = ImVec4(0.710f, 0.537f, 0.000f, 1.0f); // b58900
    g_colors.marked_bg         = IM_COL32(84, 97, 0, 255);
    g_colors.cursor_bg         = IM_COL32(38, 109, 160, 255);
    g_colors.cursor_outline    = IM_COL32(42, 161, 152, 255);          // 2aa198
    g_colors.window_bg         = ImVec4(0.000f, 0.169f, 0.212f, 1.0f);
}

// ---------------------------------------------------------------- solarized light
static void theme_solarized_light(void) {
    ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    C(ImGuiCol_Text,                  0.396f, 0.482f, 0.514f, 1.00f); // 657b83
    C(ImGuiCol_TextDisabled,          0.576f, 0.631f, 0.631f, 1.00f); // 93a1a1
    C(ImGuiCol_WindowBg,              0.992f, 0.965f, 0.890f, 1.00f); // fdf6e3
    C(ImGuiCol_ChildBg,               0.933f, 0.910f, 0.835f, 1.00f); // eee8d5
    C(ImGuiCol_PopupBg,               0.992f, 0.965f, 0.890f, 0.98f);
    C(ImGuiCol_Border,                0.576f, 0.631f, 0.631f, 0.70f); // 93a1a1
    C(ImGuiCol_FrameBg,               0.933f, 0.910f, 0.835f, 1.00f);
    C(ImGuiCol_FrameBgHovered,        0.890f, 0.859f, 0.769f, 1.00f);
    C(ImGuiCol_FrameBgActive,         0.851f, 0.812f, 0.710f, 1.00f);
    C(ImGuiCol_TitleBg,               0.933f, 0.910f, 0.835f, 1.00f);
    C(ImGuiCol_TitleBgActive,         0.992f, 0.965f, 0.890f, 1.00f);
    C(ImGuiCol_MenuBarBg,             0.933f, 0.910f, 0.835f, 1.00f);
    C(ImGuiCol_CheckMark,             0.149f, 0.545f, 0.824f, 1.00f); // 268bd2
    C(ImGuiCol_SliderGrab,            0.149f, 0.545f, 0.824f, 1.00f);
    C(ImGuiCol_SliderGrabActive,      0.200f, 0.630f, 0.870f, 1.00f);
    C(ImGuiCol_Button,                0.933f, 0.910f, 0.835f, 1.00f);
    C(ImGuiCol_ButtonHovered,         0.890f, 0.859f, 0.769f, 1.00f);
    C(ImGuiCol_ButtonActive,          0.851f, 0.812f, 0.710f, 1.00f);
    C(ImGuiCol_Header,                0.890f, 0.859f, 0.769f, 0.55f);
    C(ImGuiCol_HeaderHovered,         0.851f, 0.812f, 0.710f, 0.80f);
    C(ImGuiCol_HeaderActive,          0.780f, 0.737f, 0.627f, 0.80f);
    C(ImGuiCol_Separator,             0.576f, 0.631f, 0.631f, 0.50f);
    C(ImGuiCol_SeparatorHovered,      0.149f, 0.545f, 0.824f, 0.78f);
    C(ImGuiCol_SeparatorActive,       0.149f, 0.545f, 0.824f, 1.00f);
    C(ImGuiCol_Tab,                   0.933f, 0.910f, 0.835f, 1.00f);
    C(ImGuiCol_TabHovered,            0.149f, 0.545f, 0.824f, 0.80f);
    C(ImGuiCol_TabActive,             0.851f, 0.812f, 0.710f, 1.00f);
    C(ImGuiCol_TextSelectedBg,        0.149f, 0.545f, 0.824f, 0.35f);

    g_colors.title_focused     = ImVec4(0.300f, 0.550f, 0.750f, 1.0f);
    g_colors.title_focused_hov = ImVec4(0.360f, 0.620f, 0.820f, 1.0f);
    g_colors.title_unfocused   = ImVec4(0.933f, 0.910f, 0.835f, 1.0f); // eee8d5
    g_colors.dir               = ImVec4(0.149f, 0.545f, 0.824f, 1.0f); // 268bd2
    g_colors.exec              = ImVec4(0.420f, 0.480f, 0.000f, 1.0f);
    g_colors.diff_missing      = ImVec4(0.796f, 0.294f, 0.086f, 1.0f); // cb4b16
    g_colors.diff_differ       = ImVec4(0.710f, 0.537f, 0.000f, 1.0f); // b58900
    g_colors.marked_bg         = IM_COL32(184, 199, 140, 255);
    g_colors.cursor_bg         = IM_COL32(140, 190, 230, 255);
    g_colors.cursor_outline    = IM_COL32(42, 161, 152, 255);          // 2aa198
    g_colors.window_bg         = ImVec4(0.992f, 0.965f, 0.890f, 1.0f);
}

#undef C

void mc_apply_theme(const char* name) {
    if      (!strcmp(name, "light"))          theme_light();
    else if (!strcmp(name, "classic"))        theme_classic();
    else if (!strcmp(name, "gruvbox"))        theme_gruvbox_dark();
    else if (!strcmp(name, "gruvbox-light"))  theme_gruvbox_light();
    else if (!strcmp(name, "solarized"))      theme_solarized_dark();
    else if (!strcmp(name, "solarized-dark")) theme_solarized_dark();
    else if (!strcmp(name, "solarized-light")) theme_solarized_light();
    else                                       theme_dark();
}

const MCThemeColors* mc_theme_colors() {
    return &g_colors;
}
