set -e

if ! [ -d allib ]; then
    echo "please checkout the allib submodule (recursively)"
    exit 1
fi

if ! [ -d slowdb ]; then
    echo "please checkout the slowdb submodule"
    exit 1
fi

has_include () {
	echo "#include \"$1\"" > .temp.c
	echo "int main() {return 0;}" >> .temp.c
	if cc -c .temp.c -o .temp.o 2>/dev/null; then
		echo 1
	else
		echo 0
	fi
}

is_t2=0
is_arch=0

if [ -f /usr/src/t2-src/t2 ]; then
	is_t2=1
elif hash pacman 2>/dev/null; then
	is_arch=1
fi

if ! [ $(has_include "cjson/cJSON.h") = 1 ]; then
	echo "cjson not found"
	if [ $is_t2 = 1 ]; then
		echo sugesstion: t2 install cjson
	elif [ $is_arch = 1] ; then
		echo suggestion: pacman -S cjson
	fi
	exit 1
fi

hocon_args="-lhocon"
if ! [ $(has_include "hocon.h") = 1 ]; then
	echo hocon-c not found
	if [ $is_t2 = 1 ]; then
		echo suggestion: t2 install hocon
		exit 1
	fi
	echo automatically building hocon...
	if ! hash cmake 2>/dev/null; then
		echo need cmake for auto-building hocon!
		if [ $is_arch = 1 ]; then
			echo suggestion: pacman -S cmake
		fi
		exit 1
	fi
	if ! hash make 2>/dev/null; then
		echo need make for auto-building hocon!
		if [ $is_arch = 1 ]; then
			echo suggestion: pacman -S make
		fi
		exit 1
	fi
	new=0
	if ! [ -d hocon ]; then
		new=1
		git clone https://github.com/nanomq/hocon.git
	fi
	cd hocon
	if [ $new = 1 ]; then
		git checkout 35cc180
		git apply ../hocon-build.patch -p1
	fi
	cmake -B build -G "Unix Makefiles"
	cmake --build build
	hocon_args="$(realpath build/libhocon.a) -I$(realpath .)"
	cd ..
fi

args=$hocon_args
if [ $(has_include "zstd.h") = 1 ]; then
	args+=" -DHAS_ZSTD -lzstd"
fi

cd allib
cc build.c -DCC="\"cc\"" $(build_c/slowdb/build.sh) -o build.exe
./build.exe all.a
cd ..
cc -o a.out *.c C-Http-Server/src/*.c C-Http-Server/thread-pool/*.c allib/build/all.a $(CC=cc slowdb/build.sh) -Wno-nullability-completeness -lcurl -lcjson -DHAS_ZLIB -lz $args

mv a.out /usr/bin/t2-mirror-server
if [ -f /etc/t2-mirror-server.hocon ]; then
	echo not overwriting config. you might need to manually update the config if the format changed
else
	cp config.hocon.def /etc/t2-mirror-server.hocon
fi

if [ -d /etc/rc.d/init.d ]; then
	cp svinit_service /etc/rc.d/init.d/t2-mirror-server
fi

if [ -d /etc/systemd/system ]; then
    if ! [ -f /etc/systemd/system/t2-mirror-server.service ]; then
        cp t2-mirror-server.service /etc/systemd/system
    fi
fi
