#if defined(_WIN32)
extern "C" __declspec(dllexport) int fixture_dependency() { return 42; }
#else
extern "C" __attribute__((visibility("default"))) int fixture_dependency() { return 42; }
#endif
