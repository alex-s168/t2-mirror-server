mkdir -p data
cd allib
cc build.c -o build.exe
./build.exe all.a
cd ..
cc -g -glldb *.c C-Http-Server/src/*.c C-Http-Server/thread-pool/*.c allib/build/all.a -lcurl -lcjson -DHAS_ZLIB -DHAS_ZSTD -lz -lzstd -lhocon
