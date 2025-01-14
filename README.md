A mirror server for packe source tars in T2.

Has two operation modes (can be combined)
- local mode: recommended for in home-network use
- public mode: when you want to host a publicly accesible T2 mirror.
  Uses a cloned T2 source tree to figure out which name - url download pairs are allowed.

Depends on the following t2 packages:
- `curl`
- `hocon`

first, run `build.sh`.
then modify the `config.hocon` according to your needs,
and then start the server: `./a.out`.

now you can (on different devices) change the /usr/src/t2-src/download/Mirror-Cache to point to this server instead.

You also should apply the `download.patch` patch, to send this server more information on download. (does not matter if on public mode)
