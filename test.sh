server="http://localhost:8070"

for i in $(seq 100); do
    curl "$server/a/Aaa" &
    curl "$server/a/Aaa" &
    curl "$server/b/b" &
done
