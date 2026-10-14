# Toolchain image: devkitPro devkitA64 with libnx built from the MissionControl submodule
# (MissionControl needs a newer libnx than the released one).
#   docker build -t mc-devkit -f tools/Dockerfile MissionControl/lib/libnx
FROM devkitpro/devkita64:latest
COPY . /tmp/libnx
RUN git config --global --add safe.directory '*' \
 && make -C /tmp/libnx/nx -j"$(nproc)" \
 && make -C /tmp/libnx/nx install \
 && rm -rf /tmp/libnx
