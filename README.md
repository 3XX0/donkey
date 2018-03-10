# DONKEY

Docker build secret utility.

This project provides a simple tool to pass secrets to `docker build`.\
It does so while trying to minimize the exposure of sensitive information.

**Important: This tool requires `--network=host` to be specified in the `docker build` arguments.**



## Usage

```sh
donkey set <filename>
```

Create a one-time secret and display its identifier on standard output.\
`filename` must point to either the path of the secret or `-` for standard input.

```sh
donkey get <id> [<command>]
```

Retrieve a secret from an identifier and display its content on standard output.\
When a command is given, the secret is available through the file refered by the `DONKEY_FILE` environment variable instead.

## Example

```sh
docker build --network=host
             --build-arg FOO=$(donkey set secret.txt &)
             --build-arg BAR=$(cat secret.txt | donkey set - &)
             -f Dockerfile.sample .
```

## Build

Without Docker:
```sh
make
```

With Docker:
```
docker build -t donkey -f Dockerfile .
docker run donkey > donkey && chmod +x donkey
```
