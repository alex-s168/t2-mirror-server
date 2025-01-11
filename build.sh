mkdir -p data
clang -g -ggdb *.c C-Http-Server/src/*.c C-Http-Server/C-Thread-Pool/thpool.c -lcurl -lcjson -lhocon
