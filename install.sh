set -e
mkdir -p data
cd allib
cc build.c -o build.exe
./build.exe all.a
cd ..
cc -o a.out *.c C-Http-Server/src/*.c C-Http-Server/thread-pool/*.c allib/build/all.a -lcurl -lcjson -DHAS_ZLIB -DHAS_ZSTD -lz -lzstd -lhocon

mv a.out /usr/bin/t2-mirror-server
if [ -f /etc/t2-mirror-server.hocon ]; then
	echo not overwriting config. you might need to manually update the config if the format changed
else
	cp config.hocon.def /etc/t2-mirror-server.hocon
fi

if [ -d /etc/rc.d/init.d ]; then
	cp svinit_service /etc/rc.d/init.d/t2-mirror-server
fi
