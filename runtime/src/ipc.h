// HIPC/CMIF service framework.
#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kernel.h"

namespace nx {

struct Buffer {
    uint64_t addr = 0, size = 0;
};

struct Service;

struct IpcCtx {
    CPU* cpu = nullptr;
    uint32_t cmd = 0;
    uint32_t type = 0;
    const uint8_t* in = nullptr;  // raw arguments (after the SFCI header)
    size_t in_size = 0;
    std::vector<uint32_t> in_copy, in_move;
    uint64_t pid = 0;
    std::vector<Buffer> a, b, x, c;  // send, receive, pointer (X), receive list (C)
    std::vector<uint32_t> in_objects;  // domain objects passed in
    std::vector<std::shared_ptr<Service>> in_services;  // the interfaces passed in (domain objects or moved sessions), resolved

    std::vector<uint8_t> out;
    std::vector<uint32_t> out_copy, out_move;
    std::vector<std::shared_ptr<Service>> out_objects;

    template <typename T>
    T arg(size_t off) const {
        T v{};
        if (off + sizeof(T) <= in_size) std::memcpy(&v, in + off, sizeof(T));
        return v;
    }
    template <typename T>
    void push(const T& v) {
        size_t o = out.size();
        out.resize(o + sizeof(T));
        std::memcpy(out.data() + o, &v, sizeof(T));
    }
    void push_object(std::shared_ptr<Service> s) { out_objects.push_back(std::move(s)); }
    // Buffer of either kind for "the n-th input/output buffer" (A or X / B or C).
    Buffer in_buf(size_t n) const { return n < a.size() && a[n].addr ? a[n] : (n < x.size() ? x[n] : Buffer{}); }
    Buffer out_buf(size_t n) const { return n < b.size() && b[n].addr ? b[n] : (n < c.size() ? c[n] : Buffer{}); }
};

struct Service : std::enable_shared_from_this<Service> {
    std::string name;
    explicit Service(std::string n) : name(std::move(n)) {}
    virtual ~Service() = default;
    // Returns the result code. Unknown commands log and succeed with no output.
    virtual uint32_t handle(IpcCtx& ctx);
    std::map<uint32_t, std::function<uint32_t(IpcCtx&)>> cmds;
};

using ServiceFactory = std::function<std::shared_ptr<Service>()>;
void register_service(const std::string& name, ServiceFactory f);
std::shared_ptr<Service> make_service(const std::string& name);

struct ServiceRegistrar {
    ServiceRegistrar(const char* name, ServiceFactory f) { register_service(name, std::move(f)); }
};

std::shared_ptr<Service> make_storage(std::vector<uint8_t> data);
std::shared_ptr<Service> make_storage(std::shared_ptr<std::vector<uint8_t>> blob);  // shared, writable
// The bytes behind an IStorage made by make_storage (null for any other service).
std::shared_ptr<std::vector<uint8_t>> storage_blob(const std::shared_ptr<Service>& s);
std::shared_ptr<Service> preselected_user_storage();

// Kernel shared memory owned by a service (hid, etc.)
std::shared_ptr<KSharedMemory> make_shared_memory(const std::string& name, uint64_t size);

}  // namespace nx
