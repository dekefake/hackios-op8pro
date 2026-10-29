# source me (bash/zsh) before using xl, docker or fastboot on this Mac:  . tools/macdev/env.sh
# Stand-ins for the Linux tools xl expects (flock, timeout) and the Docker Desktop socket.
MACDEV=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)
export PATH=$MACDEV/shim:$MACDEV:$PATH
export DOCKER_HOST=unix://$HOME/.docker/run/docker.sock
