#include "application.hpp"
namespace Fundamental {
ApplicationInterface::~ApplicationInterface() {
}
bool ApplicationInterface::Load(int argc, char** argv) {
    return true;
}

bool ApplicationInterface::Init() {
    return true;
}

void ApplicationInterface::Tick() {
}

void ApplicationInterface::Exit() {
}

void Application::OverlayApplication(std::shared_ptr<ApplicationInterface>&& newImp) {
    imp = std::move(newImp);
}
} // namespace Fundamental