# Linux build/test environment for BBFS (handy on macOS without macFUSE).
#
#   docker build -t bbfs .
#   docker run --rm -it --device /dev/fuse --cap-add SYS_ADMIN bbfs
#
# Then inside the container:  make mount && ls example/mountdir
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make pkg-config libfuse-dev fuse \
 && rm -rf /var/lib/apt/lists/*

# BBFS refuses to run as root.
RUN useradd -m bb
USER bb
WORKDIR /home/bb/bbfs
COPY --chown=bb:bb . .
RUN make clean && make

CMD ["bash"]
