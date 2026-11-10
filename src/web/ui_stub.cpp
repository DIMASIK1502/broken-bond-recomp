// Message boxes without the SDL/ImGui drawer. The calls ImGui makes from
// xam_ui still need the ImGui library; there is no window to host them yet.

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>

#include <imgui.h>

namespace rex::ui {

ImGuiDialog::ImGuiDialog(ImGuiDrawer* imgui_drawer) : imgui_drawer_(imgui_drawer) {}

ImGuiDialog::~ImGuiDialog() {
  for (auto* fence : waiting_fences_) {
    fence->Signal();
  }
}

void ImGuiDialog::Then(rex::thread::Fence* fence) { waiting_fences_.push_back(fence); }

void ImGuiDialog::Close() { has_close_pending_ = true; }

ImGuiIO& ImGuiDialog::GetIO() { return ImGui::GetIO(); }

void ImGuiDialog::Draw() {
  OnDraw(GetIO());
  if (has_close_pending_) {
    OnClose();
    delete this;
  }
}

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer, std::string title,
                                         std::string body) {
  (void)imgui_drawer;
  (void)title;
  (void)body;
  return nullptr;
}

}  // namespace rex::ui
