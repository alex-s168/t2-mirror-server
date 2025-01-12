Depends on the following t2 packages:
- `curl`
- `hocon`

first modify the `config.hocon` according to your needs.
run `build.sh`, and then start the server: `./a.out`.

now you can (on different devices) change the /usr/src/t2-src/download/Mirror-Cache to point to this server instead.

You also should apply the `download.patch` patch, to send this server more information on download.
