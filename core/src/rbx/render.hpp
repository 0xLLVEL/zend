#pragma once

namespace zend::rbx::render {

// Starts the watcher that finds RenderJob's swapchain and installs the
// Present/ResizeBuffers hooks. Idempotent.
void start();

// Restores the swapchain vtable if it still points at our clone. Called from
// DllMain detach; makes no allocations and waits for nothing.
void stop();

} // namespace zend::rbx::render
