#!/usr/bin/env bash
#
# ci/pgdg.sh: build and test pg_gaps against one PGDG PostgreSQL major on
# Debian or Ubuntu, the way a distribution user builds it.
#
# Usage: ci/pgdg.sh MAJOR
#
# Adds the PGDG apt repository, installs the server and its headers, builds
# with gcc and -Werror, runs the ASCII and META.json checks, installs, and
# runs the regression suite, the pg_dump test and the upgrade test in a
# throwaway cluster (pg_virtualenv).  The upgrade test builds the 0.1.0
# starting points from their commits, so the checkout needs its history.
# Runs as root (a CI container) or as a user with passwordless sudo (a
# GitHub runner).  Both forges' CI call this script, so that their gates
# cannot drift apart.
set -euo pipefail

major="${1:?usage: ci/pgdg.sh MAJOR}"
sudo=
if [ "$(id -u)" -ne 0 ]; then
	sudo=sudo
fi
export DEBIAN_FRONTEND=noninteractive

# The PGDG repository.  GitHub's runners carry a Microsoft apt source that
# intermittently serves a broken InRelease and fails apt-get update.
$sudo rm -f /etc/apt/sources.list.d/microsoft*.list /etc/apt/sources.list.d/microsoft*.sources
$sudo apt-get update -q
$sudo apt-get install -y -q --no-install-recommends ca-certificates curl gnupg lsb-release
keyring=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
$sudo install -d "$(dirname "$keyring")"
$sudo curl -fsSL --retry 5 -o "$keyring" https://www.postgresql.org/media/keys/ACCC4CF8.asc
echo "deb [signed-by=$keyring] https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" |
	$sudo tee /etc/apt/sources.list.d/pgdg.list >/dev/null
$sudo apt-get update -q
$sudo apt-get install -y -q --no-install-recommends \
	"postgresql-$major" "postgresql-server-dev-$major" gcc make libc6-dev git

pgc="/usr/lib/postgresql/$major/bin/pg_config"
make PG_CONFIG="$pgc" COPT=-Werror
make PG_CONFIG="$pgc" check-ascii check-meta
$sudo make PG_CONFIG="$pgc" install

if ! pg_virtualenv -v "$major" make PG_CONFIG="$pgc" installcheck; then
	cat regression.diffs 2>/dev/null || true
	exit 1
fi
pg_virtualenv -v "$major" bash test/dump/run.sh
if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
	git fetch -q --unshallow
fi
extdir="$("$pgc" --sharedir)/extension"
$sudo bash test/upgrade/stage.sh "$pgc" "$PWD/upgrade-build" "$("$pgc" --pkglibdir)" "$extdir"
PG_GAPS_EXTDIR="$extdir" pg_virtualenv -v "$major" bash test/upgrade/run.sh
echo "ci/pgdg.sh: PostgreSQL $major passed"
