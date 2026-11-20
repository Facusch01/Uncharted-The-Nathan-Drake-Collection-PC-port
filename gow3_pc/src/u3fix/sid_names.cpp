// Uncharted 3 1.02, 20 Nov 2026: name ids instead of "(no debug memory)" in the game's messages.
//
// Names in the game data are 32-bit ids; the function that turns an id back into text (image 0xf38f40) has no name
// table in the retail game and answers "(no debug memory)". The script messages printed by script_errors_log.cpp then
// say which kind of thing was not found, but not which one. The function now runs as before; only when its answer is
// that text does it give "#" and the id in hex instead (from a small ring of buffers per thread), so the id can be
// matched against the names in the level's data. Found by its bytes.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace u3fix {

namespace {

__attribute__((sysv_abi)) const char *name_or_id(uint32_t id, const char *name) {
    if (!name || std::strcmp(name, "(no debug memory)") != 0) return name;
    thread_local char ring[8][16];
    thread_local unsigned next = 0;
    char *out = ring[next++ & 7];
    std::snprintf(out, 16, "#%08x", id);
    return out;
}

// push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x18; mov r14d,esi; mov ebx,edi; lea r15,[canary]
constexpr unsigned char kSite[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec,
                                   0x18, 0x41, 0x89, 0xf6, 0x89, 0xfb, 0x4c, 0x8d, 0x3d, 0xcf, 0xe1, 0x7a};
constexpr unsigned kPrologue = 6;   // push rbp; mov rbp,rsp; push r15

}  // namespace

bool install_sid_names(unsigned char *image, uint64_t size) {
    const uint64_t at = find_unique(image, size, kSite, sizeof(kSite));
    if (at == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    unsigned n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) { for (unsigned char ch : b) stub[n++] = ch; };
    auto abs_jump = [&](uint64_t to) { put({0xff, 0x25, 0, 0, 0, 0}); std::memcpy(stub + n, &to, 8); n += 8; };
    // the entry: keep the id, run the original function (its prologue here, then on in the image), fix its answer
    put({0x57, 0x56, 0x48, 0x83, 0xec, 0x08});   // push rdi; push rsi; sub rsp,8 (aligned for the call)
    put({0xe8, 0, 0, 0, 0});                     // call trampoline (rel32 filled in below)
    const unsigned call_end = n;
    put({0x48, 0x83, 0xc4, 0x08, 0x5e, 0x5f});   // add rsp,8; pop rsi; pop rdi
    put({0x48, 0x83, 0xec, 0x08});               // sub rsp,8
    put({0x48, 0x89, 0xc6});                     // mov rsi,rax (the answer); rdi = the id
    put({0x48, 0xb8});                           // mov rax, name_or_id
    const uint64_t fixer = reinterpret_cast<uint64_t>(&name_or_id);
    std::memcpy(stub + n, &fixer, 8);
    n += 8;
    put({0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0xc3});   // call rax; add rsp,8; ret
    const unsigned trampoline = n;
    std::memcpy(stub + n, image + at, kPrologue);      // the original prologue
    n += kPrologue;
    abs_jump(reinterpret_cast<uint64_t>(image + at + kPrologue));
    const int32_t rel = static_cast<int32_t>(trampoline - call_end);
    std::memcpy(stub + call_end - 4, &rel, 4);
    if (!finish_stub(stub) || !jump_to(image, at, kPrologue, stub)) return false;
    std::printf("Uncharted 3: names without debug memory are shown as their ids (%#llx)\n",
                static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix
