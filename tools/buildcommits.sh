#!/usr/bin/env bash
err() {
  echo 1>&2 "error: $@"
  exit
}

out=
make_args=
cmd=
log=/dev/null
while getopts o:m:c:v opt; do
  case $opt in
  o) out=$(realpath -m "$OPTARG") ;;
  m) make_args=$OPTARG ;;
  c) cmd=$OPTARG ;;
  v) log=/dev/stdout ;;
  *) exit 1 ;;
  esac
done
shift $((OPTIND - 1))

commits=()
for arg; do
  if [[ $arg == *..* ]]; then
    commits+=($(git rev-list --reverse "$arg"))
  else
    commits+=($(git rev-parse --verify "$arg^{commit}"))
  fi
done

wtbase=$(mktemp -d) || err "mktemp failed"
trap "rm -rf "$wtbase"" INT TERM EXIT

for c in "${commits[@]}"; do
  name=$(git log -1 --date=format:%Y-%m-%d-T%H%M%S --format=%cd-%h "$c")
  wt="$wtbase/$name"
  o=
  [ -n "$out" ] && o="$out/$name"
  #echo "name=${name}, wt=${wt}, o=${o}"

  git worktree add --quiet --detach "$wt" "$c"

  make -C "$wt" -j"$(nproc)" ${o:+"O=$o"} $make_args >"$log"
  if [ -n "$cmd" ]; then
    echo "Running command for: ${name}"
    (cd "$wt" && O=$o COMMIT=$c bash -c "$cmd")
  fi

  git worktree remove --force "$wt"
done
