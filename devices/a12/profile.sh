# Source with bash. Values are shlex-quoted by our own read-only profile helper.
_A12_PROFILE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_A12_PROFILE_ENV="$(python3 "$_A12_PROFILE_DIR/profile.py" --shell)" || return 2
eval "$_A12_PROFILE_ENV"
unset _A12_PROFILE_DIR _A12_PROFILE_ENV
