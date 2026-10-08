// Exercise the actual hook and client update with a synthetic provider. No game,
// Wine prefix, rendering, or installed music/profile data is used.
#include "Extension/Music/local_music_ui.h"
#include "Extension/Customization/local_customization_runtime.h"
#include <cstdio>
#include <thread>

// Native dependencies are replaced only at the game boundary; queueing, identity,
// lease acquisition, retry and transition cancellation run production code.
namespace dingosdk::profile_runtime {
LocalRuntime& local_runtime() { static auto* value = new LocalRuntime; return *value; }
CosmeticRuntime& cosmetic_runtime() { static CosmeticRuntime value; return value; }
CosmeticCatalogFunctions& cosmetic_catalog_functions() { static CosmeticCatalogFunctions value; return value; }
bool read_music_catalog(MusicCatalog&) { return false; } // Assets have not registered yet.
bool install_playlist_lookup_safety(std::uintptr_t) noexcept { return false; }
void cosmetic_release(CosmeticShared& value) noexcept {
    if (value.control) {
        auto* counts = reinterpret_cast<volatile LONG*>(static_cast<std::byte*>(value.control) + 8);
        InterlockedDecrement(counts);
        InterlockedDecrement(counts + 1);
    }
    value = {};
}
bool identifier(const void*, std::string&) { throw std::runtime_error("Unexpected publication"); }
std::uint32_t cosmetic_hash(std::string_view) { throw std::runtime_error("Unexpected publication"); }
bool cosmetic_text(std::uintptr_t, std::string&) { throw std::runtime_error("Unexpected publication"); }
void cosmetic_varint(std::string&, std::uint64_t) { throw std::runtime_error("Unexpected publication"); }
void cosmetic_wire_number(std::string&, unsigned, std::uint64_t) { throw std::runtime_error("Unexpected publication"); }
void cosmetic_wire_string(std::string&, unsigned, std::string_view) { throw std::runtime_error("Unexpected publication"); }
}

using namespace dingosdk;
using namespace dingosdk::profile_runtime;
namespace {
unsigned copies{}, destroyed{}, forwarded{};
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void copy_delegate(void* out, const void* in) {
    ++copies;
    *static_cast<std::uintptr_t*>(out) = *static_cast<const std::uintptr_t*>(in);
}
void destroy_delegate(void* value) {
    ++destroyed;
    *static_cast<std::uintptr_t*>(value) = 0;
}
void initialize(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, const std::uint32_t*, const void*) {
    ++forwarded;
}
void queue() {
    const std::uintptr_t callback = 42;
    const std::uint32_t favorites = 7;
    music_ui_initialize_hook(1, 2, 3, 4, 5, 6, 7, &favorites, &callback);
}
void regression() {
    // Reserve a synthetic image; only the two singleton slots are committed data.
    const auto image_size = addr::local_music::ui_manager + 4096;
    auto* image = static_cast<std::byte*>(VirtualAlloc(nullptr, image_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(image != nullptr, "Could not allocate test image");
    std::array<std::uintptr_t, 80> provider{};
    std::array<std::uintptr_t, 2> owner{};
    auto& runtime = local_runtime();
    auto& ui = music_ui_runtime();
    runtime.base = reinterpret_cast<std::uintptr_t>(image);
    runtime.active.store(true);
    cosmetic_runtime().update_thread = GetCurrentThreadId();
    provider[0] = runtime.base + addr::local_music::ui_manager_vtable;
    provider[0x80 / 8] = 0x1234;
    provider[0x58 / 8] = reinterpret_cast<std::uintptr_t>(owner.data());
    auto* counts = reinterpret_cast<LONG*>(owner.data() + 1);
    counts[0] = 1; counts[1] = 1;
    const auto manager = reinterpret_cast<std::uintptr_t>(provider.data());
    std::memcpy(image + addr::local_music::ui_manager, &manager, sizeof(manager));
    game::native_data().values.copy_delegate = copy_delegate;
    game::native_data().values.destroy_delegate = destroy_delegate;
    ui.functions.initialize = initialize;

    // Regression: a map-load worker's request must not fall through to the online
    // provider merely because an earlier client tick established another thread.
    std::thread worker(queue); worker.join();
    check(ui.pending && copies == 1 && forwarded == 0 && counts[1] == 2,
        "Worker initialization was forwarded or lost its owned callback/weak reference");
    update_music_catalog(); // Registry absent: keep waiting on the client thread.
    check(ui.pending && !ui.updating && destroyed == 0 && counts[0] == 1 && counts[1] == 2,
        "Client tick discarded a worker request while waiting for music assets");
    std::thread wrong_thread(update_music_catalog); wrong_thread.join();
    check(ui.pending && destroyed == 0, "Non-client tick consumed pending initialization");

    music_ui_before_level_transition(14);
    check(!ui.pending && destroyed == 1 && counts[1] == 1 && ui.blocked,
        "Map unload failed to release the old callback before its script disappears");
    music_ui_before_level_transition(3);
    check(destroyed == 1, "Repeated unload destroyed a callback twice");
    music_ui_before_level_transition(4);
    std::thread next_worker(queue); next_worker.join();
    check(ui.pending && forwarded == 0, "Next map's constructor could not queue music");
    update_music_catalog();
    check(ui.pending != nullptr, "Next map lost initialization while assets were loading");

    // Reentrant unload also invalidates a request already moved out of the queue.
    auto in_flight = std::move(ui.pending);
    music_ui_before_level_transition(22);
    check(!music_ui_current(*in_flight), "In-flight request survived map teardown");
    in_flight.reset();
    check(destroyed == 2 && counts[1] == 1, "In-flight request leaked native ownership");
    music_ui_before_level_transition(16);
    queue();
    provider[0x80 / 8] = 0x5678;
    update_music_catalog();
    check(!ui.pending && destroyed == 3, "Replaced provider model retained stale contexts");
    queue();
    queue();
    check(ui.pending && destroyed == 4, "New initialization failed to supersede the old one");
    music_ui_before_level_transition(24);
    music_ui_before_level_transition(4);
    check(ui.blocked && !ui.pending && destroyed == 5 && counts[1] == 1,
        "Shutdown resumed or leaked music requests");
    runtime.active.store(false);
    runtime.base = 0;
    VirtualFree(image, 0, MEM_RELEASE);
}
}
int main() {
    try { regression(); std::puts("Music map-transition regressions passed."); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
