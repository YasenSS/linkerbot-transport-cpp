#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifdef Py_GIL_DISABLED
#error "This proof module requires a conventional GIL-enabled CPython build"
#endif
#include <array>
#include <atomic>
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/runtime.hpp>
#include <memory>
#include <mutex>
#include <type_traits>

namespace {
    using namespace linkerbot;
    using namespace std::chrono_literals;
    constexpr const char *kContext = "transport_probe.context";
    constexpr const char *kReceipt = "transport_probe.receipt";
    constexpr size_t kCredits = 8;
    struct Unlock {
        PyThreadState *state = PyEval_SaveThread();
        ~Unlock() {
            PyEval_RestoreThread(state);
        }
        Unlock() = default;
        Unlock(const Unlock &) = delete;
        Unlock &operator=(const Unlock &) = delete;
    };
    struct Counters {
        std::atomic<int> opens{0};
        std::atomic<size_t> frames{0}, active{0}, maximum{0}, credits{0};
        std::mutex mutex;
        can::FakeTransport *backend = nullptr;
    };
    struct Backend : can::FakeTransport {
        std::shared_ptr<Counters> owner;
        explicit Backend(std::shared_ptr<Counters> counters) : owner(std::move(counters)) {}
        ~Backend() override;
        void write(const can::Message &message) override;
    };
    struct Context {
        transport::TransportRuntime runtime;
        std::shared_ptr<Counters> counters = std::make_shared<Counters>();
        std::atomic<size_t> accepted{0};
        std::array<std::shared_ptr<transport::DeviceSession>, 2> devices;
        explicit Context(const std::string &key) {
            transport::ChannelSpec channel;
            channel.key.backend = "fake";
            channel.key.endpoint = key;
            channel.configuration_fingerprint = "preintegration/fake-v1";
            auto factory = [&] {
                ++counters->opens;
                auto value = std::make_unique<Backend>(counters);
                counters->backend = value.get();
                return value;
            };
            devices[0] = runtime.open_device(channel, {"hand-probe", {}}, factory);
            devices[1] = runtime.open_device(channel, {"arm-probe", {}}, factory);
        }
        ~Context() {
            {
                std::lock_guard<std::mutex> lock(counters->mutex);
                if (counters->backend != nullptr) {
                    counters->backend->release_tx_gate();
                }
            }
            devices = {};
        }
    };
    Backend::~Backend() {
        std::lock_guard<std::mutex> lock(owner->mutex);
        owner->backend = nullptr;
    }
    void Backend::write(const can::Message &message) {
        const auto depth = ++owner->active;
        auto high = owner->maximum.load();
        while (high < depth && !owner->maximum.compare_exchange_weak(high, depth)) {
        }
        struct Exit {
            Counters *owner;
            ~Exit() {
                --owner->active;
            }
        } exit{owner.get()};
        can::FakeTransport::write(message);
        ++owner->frames;
    }
    struct Slot {
        std::shared_ptr<Context> context;
        std::shared_ptr<Counters> counters;
        transport::SendReceipt receipt;
        bool committed = false;
        std::atomic<size_t> waiters{0};
        std::atomic<bool> credit_active{true};
        explicit Slot(std::shared_ptr<Context> value)
            : context(std::move(value)), counters(context->counters) {}
        void release_credit() {
            if (credit_active.exchange(false)) {
                --counters->credits;
            }
        }
        ~Slot() {
            release_credit();
        }
    };
    static_assert(std::is_nothrow_move_assignable_v<transport::SendReceipt>);
    // Test-only allocator wrapper, active only during capsule creation under the
    // conventional GIL. No other interpreter is supported by this probe.
    struct CapsuleAllocationFault {
        PyMemAllocatorEx previous{};
        std::atomic<bool> hit{false};
        static bool fail(CapsuleAllocationFault &state) {
            return !state.hit.exchange(true);
        }
        static void *allocate(void *context, size_t bytes) {
            auto &state = *static_cast<CapsuleAllocationFault *>(context);
            return fail(state) ? nullptr : state.previous.malloc(state.previous.ctx, bytes);
        }
        static void *zeroed(void *context, size_t count, size_t bytes) {
            auto &state = *static_cast<CapsuleAllocationFault *>(context);
            return fail(state) ? nullptr : state.previous.calloc(state.previous.ctx, count, bytes);
        }
        // CPython's allocator ABI fixes the context/pointer parameter order.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        static void *resize(void *context, void *memory, size_t bytes) {
            auto &state = *static_cast<CapsuleAllocationFault *>(context);
            return fail(state) ? nullptr
                               : state.previous.realloc(state.previous.ctx, memory, bytes);
        }
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        static void release(void *context, void *memory) {
            auto &state = *static_cast<CapsuleAllocationFault *>(context);
            state.previous.free(state.previous.ctx, memory);
        }
        CapsuleAllocationFault() {
            PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &previous);
            PyMemAllocatorEx wrapper{this, allocate, zeroed, resize, release};
            PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &wrapper);
        }
        ~CapsuleAllocationFault() {
            PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &previous);
        }
        CapsuleAllocationFault(const CapsuleAllocationFault &) = delete;
        CapsuleAllocationFault &operator=(const CapsuleAllocationFault &) = delete;
    };
    using Owner = std::shared_ptr<Context>;
    void destroy_context(PyObject *capsule) {
        auto *value = static_cast<Owner *>(PyCapsule_GetPointer(capsule, kContext));
        if (value != nullptr) {
            Unlock unlock;
            std::unique_ptr<Owner> reclaimed(value);
        }
    }
    void destroy_receipt(PyObject *capsule) {
        auto *value = static_cast<Slot *>(PyCapsule_GetPointer(capsule, kReceipt));
        if (value != nullptr) {
            Unlock unlock;
            std::unique_ptr<Slot> reclaimed(value);
        }
    }
    Owner context(PyObject *capsule) {
        auto *value = static_cast<Owner *>(PyCapsule_GetPointer(capsule, kContext));
        if (value == nullptr) {
            throw can::ValidationError("invalid context capsule");
        }
        return *value;
    }
    Slot &slot(PyObject *capsule) {
        auto *value = static_cast<Slot *>(PyCapsule_GetPointer(capsule, kReceipt));
        if (value == nullptr) {
            throw can::ValidationError("invalid receipt capsule");
        }
        return *value;
    }
    PyObject *create(PyObject *, PyObject *args) {
        const char *key = nullptr;
        if (!PyArg_ParseTuple(args, "s", &key)) {
            return nullptr;
        }
        auto owner = std::make_unique<Owner>();
        {
            Unlock unlock;
            *owner = std::make_shared<Context>(key);
        }
        auto *capsule = PyCapsule_New(owner.get(), kContext, destroy_context);
        if (capsule != nullptr) {
            [[maybe_unused]] auto *transferred = owner.release();
        } else {
            Unlock unlock;
            owner.reset();
        }
        return capsule;
    }
    PyObject *prepare(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        int fail_capsule = 0;
        if (!PyArg_ParseTuple(args, "O|i", &capsule, &fail_capsule)) {
            return nullptr;
        }
        auto owner = context(capsule);
        if (fail_capsule != 0) {
            auto *interpreter = PyThreadState_GetInterpreter(PyThreadState_Get());
            auto *thread = PyInterpreterState_ThreadHead(interpreter);
            if (PyInterpreterState_Next(PyInterpreterState_Head()) != nullptr ||
                thread != PyThreadState_Get() || PyThreadState_Next(thread) != nullptr) {
                throw can::ValidationError(
                    "allocator fault requires one interpreter and one Python thread");
            }
        }
        if (owner->counters->credits.fetch_add(1) >= kCredits) {
            --owner->counters->credits;
            PyErr_SetString(PyExc_BufferError, "receipt credits exhausted");
            return nullptr;
        }
        std::unique_ptr<Slot> value;
        try {
            value = std::make_unique<Slot>(owner);
        } catch (...) {
            --owner->counters->credits;
            throw;
        }
        PyObject *receipt = nullptr;
        if (fail_capsule != 0) {
            CapsuleAllocationFault fault;
            receipt = PyCapsule_New(value.get(), kReceipt, destroy_receipt);
        } else {
            receipt = PyCapsule_New(value.get(), kReceipt, destroy_receipt);
        }
        if (receipt != nullptr) {
            [[maybe_unused]] auto *transferred = value.release();
        }
        return receipt;
    }
    PyObject *submit(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        int device = 0;
        int fail_after_accept = 0;
        if (!PyArg_ParseTuple(args, "Oii", &capsule, &device, &fail_after_accept)) {
            return nullptr;
        }
        auto &value = slot(capsule);
        if (value.committed) {
            throw can::ValidationError("receipt already submitted");
        }
        if (device < 0 || device > 1) {
            throw can::ValidationError("invalid device");
        }
        // No callback, Python allocation or new receipt wrapper follows native admission.
        can::Message frame;
        frame.id = 0x100u + static_cast<uint32_t>(device);
        value.receipt = value.context->devices[static_cast<size_t>(device)]->submit({{frame}});
        value.committed = true;
        ++value.context->accepted;
        if (fail_after_accept != 0) {
            throw std::bad_alloc();
        }
        Py_RETURN_NONE;
    }
    PyObject *committed(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        if (!PyArg_ParseTuple(args, "O", &capsule)) {
            return nullptr;
        }
        return PyBool_FromLong(static_cast<long>(slot(capsule).committed));
    }
    PyObject *wait(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        int milliseconds = 0;
        if (!PyArg_ParseTuple(args, "Oi", &capsule, &milliseconds)) {
            return nullptr;
        }
        auto &value = slot(capsule);
        ++value.waiters;
        struct LeaveWait {
            Slot *value;
            ~LeaveWait() {
                --value->waiters;
            }
        } leave{&value};
        bool done = false;
        {
            Unlock unlock;
            try {
                (void)value.receipt.wait_result_until(transport::Clock::now() +
                                                      std::chrono::milliseconds(milliseconds));
                done = true;
            } catch (const can::TimeoutError &) {
                done = false;
            }
        }
        return PyBool_FromLong(static_cast<long>(done));
    }
    PyObject *retire(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        if (!PyArg_ParseTuple(args, "O", &capsule)) {
            return nullptr;
        }
        auto &value = slot(capsule);
        if (!value.committed || !value.receipt.completed()) {
            throw can::ValidationError("only terminal receipts may release completion ownership");
        }
        value.release_credit();
        auto owner = std::move(value.context);
        {
            Unlock unlock;
            owner.reset();
        }
        Py_RETURN_NONE;
    }
    PyObject *waiting(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        if (!PyArg_ParseTuple(args, "O", &capsule)) {
            return nullptr;
        }
        return PyBool_FromLong(static_cast<long>(slot(capsule).waiters.load() != 0));
    }
    PyObject *result(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        if (!PyArg_ParseTuple(args, "O", &capsule)) {
            return nullptr;
        }
        const auto value = slot(capsule).receipt.result();
        return Py_BuildValue("(iKKK)", static_cast<int>(value.outcome),
                             static_cast<unsigned long long>(value.sent_frames),
                             static_cast<unsigned long long>(value.generation),
                             static_cast<unsigned long long>(value.channel_instance));
    }
    PyObject *gate(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        int action = 0;
        if (!PyArg_ParseTuple(args, "Oi", &capsule, &action)) {
            return nullptr;
        }
        auto owner = context(capsule);
        bool entered = true;
        {
            Unlock unlock;
            std::lock_guard<std::mutex> lock(owner->counters->mutex);
            if (owner->counters->backend == nullptr) {
                throw can::ClosedError("backend closed");
            }
            if (action == 0) {
                owner->counters->backend->arm_tx_gate();
            } else if (action == 1) {
                entered = owner->counters->backend->wait_until_tx_entered(2s);
            } else if (action == 2) {
                owner->counters->backend->release_tx_gate();
            } else {
                throw can::ValidationError("invalid gate action");
            }
        }
        return PyBool_FromLong(static_cast<long>(entered));
    }
    PyObject *close(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        int device = 0;
        if (!PyArg_ParseTuple(args, "Oi", &capsule, &device)) {
            return nullptr;
        }
        if (device < 0 || device > 1) {
            throw can::ValidationError("invalid device");
        }
        auto owner = context(capsule);
        {
            Unlock unlock;
            owner->devices[static_cast<size_t>(device)]->close_until(transport::Clock::now() + 3s);
        }
        Py_RETURN_NONE;
    }
    PyObject *stats(PyObject *, PyObject *args) {
        PyObject *capsule = nullptr;
        if (!PyArg_ParseTuple(args, "O", &capsule)) {
            return nullptr;
        }
        auto owner = context(capsule);
        return Py_BuildValue(
            "(iKKKKKK)", owner->counters->opens.load(),
            static_cast<unsigned long long>(owner->accepted.load()),
            static_cast<unsigned long long>(owner->counters->frames.load()),
            static_cast<unsigned long long>(owner->counters->maximum.load()),
            static_cast<unsigned long long>(owner->counters->credits.load()),
            static_cast<unsigned long long>(owner->devices[0]->health().channel_instance),
            static_cast<unsigned long long>(owner->devices[1]->health().channel_instance));
    }
    template <PyObject *(*Function)(PyObject *, PyObject *)>
    PyObject *checked(PyObject *self, PyObject *args) {
        try {
            return Function(self, args);
        } catch (const std::bad_alloc &) {
            return PyErr_NoMemory();
        } catch (const can::ValidationError &error) {
            PyErr_SetString(PyExc_ValueError, error.what());
        } catch (const std::exception &error) {
            PyErr_SetString(PyExc_RuntimeError, error.what());
        } catch (...) {
            PyErr_SetString(PyExc_RuntimeError, "non-standard native exception");
        }
        return nullptr;
    }
} // namespace

// CPython requires this exported initialization symbol.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
PyMODINIT_FUNC PyInit__transport_probe() {
    if (PyInterpreterState_GetID(PyThreadState_GetInterpreter(PyThreadState_Get())) != 0) {
        PyErr_SetString(PyExc_ImportError, "proof module supports only the main interpreter");
        return nullptr;
    }
    static PyMethodDef methods[] = {{"create", checked<create>, METH_VARARGS, nullptr},
                                    {"prepare", checked<prepare>, METH_VARARGS, nullptr},
                                    {"submit", checked<submit>, METH_VARARGS, nullptr},
                                    {"committed", checked<committed>, METH_VARARGS, nullptr},
                                    {"wait", checked<wait>, METH_VARARGS, nullptr},
                                    {"retire", checked<retire>, METH_VARARGS, nullptr},
                                    {"waiting", checked<waiting>, METH_VARARGS, nullptr},
                                    {"result", checked<result>, METH_VARARGS, nullptr},
                                    {"gate", checked<gate>, METH_VARARGS, nullptr},
                                    {"close", checked<close>, METH_VARARGS, nullptr},
                                    {"stats", checked<stats>, METH_VARARGS, nullptr},
                                    {nullptr, nullptr, 0, nullptr}};
    static PyModuleDef module = {PyModuleDef_HEAD_INIT,
                                 "_transport_probe",
                                 nullptr,
                                 -1,
                                 methods,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr};
    return PyModule_Create(&module);
}
