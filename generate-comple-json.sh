rm -rf temp-build && mkdir temp-build && cd temp-build && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS="ON" .. && cd .. && mv temp-build/compile_commands.json ./build