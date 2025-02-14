set -e
if [ -z $1 ]; then
    echo expected argument: version
    exit 1
fi

mkdir downcache-$1

files="$(git ls-files)"

submodule () {
    files+=" $(cd "$1" && git ls-files | awk '{print "'"$1"'/"$1}')"
}

submodule "C-Http-Server"
submodule "allib"
submodule "allib/build_c"
submodule "allib/build_c/slowdb"

for f in $files; do
    if [ -f $f ]; then
        if ! [ -z $(dirname "$f") ]; then
            mkdir -p downcache-$1/$(dirname "$f")
        fi
        cp $f downcache-$1/$(dirname "$f")/
    fi
done
tar -czf downcache-$1.tar.gz downcache-$1
rm -r downcache-$1
