# DONKEY

Docker build secret utility.

This project provides a simple tool to pass secrets to `docker build`.\
It does so while trying to minimize the exposure of sensitive information.

**Important: This tool requires `--network=host` to be specified in the `docker build` arguments.**

## Install

```sh
wget -P /usr/local/bin https://github.com/3XX0/donkey/releases/download/v1.1.0/donkey
chmod +x /usr/local/bin/donkey
```

## Usage

```sh
donkey set <filename> [<count>]
```

Create a one-time secret (by default) and display its identifier on standard output.\
`filename` must point to either the path of the secret or `-` for standard input.
In the case where `count` is specified, the secret will be made available exactly n-times instead,
the exception being that it can not be used with the standard input modifier.

```sh
donkey get <id> [<command>]
```

Retrieve a secret from an identifier and display its content on standard output.\
When a command is given, the secret is available through the file refered by the `DONKEY_FILE` environment variable instead.

## Example

```sh
docker build --network=host
             --build-arg FOO=$(cat secret.txt | donkey set - &)
             --build-arg BAR=$(donkey set secret.txt 2 &)
             -f Dockerfile.sample .
```

## Build

Without Docker:
```sh
make
```

With Docker:
```
make docker
```

## How does it work?

1. The command `donkey set` creates an abstract unix socket (see unix(7)) and displays its address on standard output. It then  waits for an incoming connection from `root:root`.

2. The command `donkey get` connects to the above address from within the `docker build` context, thus establishing a communication channel crossing the container boundary. Note that this is only possible with `--network=host` since the network namespace would isolate the abstract socket.

3. The secret file is then mapped into memory and sent through the socket using splice(2)

4. An unnamed temporary file is created inside the container under `/dev/shm` to store the secret. This file is advertized through the environment variable `DONKEY_FILE` and remains valid for the duration of the command specified in the `donkey get` arguments (`sh -c` in the example).
