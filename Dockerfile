FROM alpine

RUN apk add --no-cache gcc make musl-dev

COPY . /tmp
WORKDIR /tmp

CMD make --quiet OUTPUT_OPTION='-o /dev/stdout'
