A mirror server for packe source tars in T2.

this is a tool for hosting a publicly accesible T2 mirror.

It Uses a cloned T2 source tree to figure out which name - url download pairs are allowed.

Depends on the following T2 packages (if built on T2):
- `curl`
- `hocon`

## installing on T2
`t2 install t2-mirror-server`

If you want to install from source, just execute `./install.sh`

Now it installed a SysV-init service which can be start/stopped via `rc t2-mirror-server start`.
To see the logs, you can `cat /var/logs/init.msg` (which will print the logs of all sysvinit services)

## installing on systemd-based systems
You can also install and run t2-mirror-server on systemd-based systems.

First, run `./install.sh`, which will copy all relevant files.

After installation, you can start/stop the service using:
```
systemctl start t2-mirror-server
systemctl stop t2-mirror-server
```
To enable the service to start automatically at boot:
```
systemctl enable t2-mirror-server
```
Logs can be viewed with:
```
journalctl -u t2-mirror-server
```

## Usage
The config can be changed in `/etc/t2-mirror-server.hocon`.

now you can (on different devices) change the /usr/src/t2-src/download/Mirror-Cache to point to this server instead:
```
http://my-server:8070 25-svn
```

If you want to host this on a public server, it is recommended to use a proxy like nginx.
