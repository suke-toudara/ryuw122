# Host compile-check stubs

Minimal stand-ins for the ESP-IDF headers used by `components/ryuw122`.

They exist so `test/host/run_compile_check.sh` can compile the driver with a
plain host compiler and catch syntax, type and format-string mistakes without a
full ESP-IDF installation. They are **not** a simulator and **not** a substitute
for `idf.py build`: the real headers decide the final behaviour.
