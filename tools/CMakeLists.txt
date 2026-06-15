# tools — developer debug aids (not shipped). debug_translate is a synchronous
# harness for stepping through the dialect translator in CLion/gdb/lldb. Links
# only the `provider` library (pure translation, no net/gateway/event loop).

add_executable(debug_translate debug_translate.cpp)
target_link_libraries(debug_translate PRIVATE provider)
