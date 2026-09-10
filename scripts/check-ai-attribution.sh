#!/bin/sh
# Reject AI authorship trailers and AI git identities.
# Used by commit-msg, pre-push, and CI.
set -eu

TRAILER_PATTERN='^[[:space:]]*(co[- ]?authored[- ]by|assisted[- ]by|generated[- ]by|ai[- ]assisted[- ]by|claude-session|coautor(a)?|asistido[- ]por|generado[- ]por)[[:space:]]*:'
GENERATED_PATTERN='generated with[[:space:]]*\[?(claude|cursor|codex|copilot|gemini|grok|chatgpt|aider|devin|windsurf)'
# Keep names tight: "Claude Shannon" must not match.
AI_NAME_PATTERN='^(claude|claude[[:space:]]+(code|opus|sonnet|haiku).*|anthropic.*|cursor([[:space:]]+agent)?|github[[:space:]]+copilot|copilot|codex|chatgpt|openai|gemini([[:space:]]+cli)?|grok([[:space:]]+build)?|aider|devin|windsurf|amazon[[:space:]]+q|cody|codeium|cline|tabnine|opencode)$'
AI_EMAIL_PATTERN='(noreply@anthropic\.com|cursoragent@cursor\.com|copilot@github\.com|[0-9]+\+(copilot|github-copilot|copilot-swe-agent)(\[bot\])?@users\.noreply\.github\.com|(github-copilot|copilot-swe-agent|devin-ai-integration)\[bot\]@users\.noreply\.github\.com|aider@aider\.chat|grok@x\.ai|noreply@openai\.com|gemini-code-assist|noreply@x\.ai)'

die() {
    printf '%s\n' "$@" >&2
    exit 1
}

message_is_forbidden() {
    printf '%s\n' "$1" | grep -Eiq "$TRAILER_PATTERN" && return 0
    printf '%s\n' "$1" | grep -Eiq "$GENERATED_PATTERN" && return 0
    return 1
}

ident_is_forbidden() {
    ident=$1
    name=${ident%% <*}
    email=$ident
    case "$ident" in
        *'<'*'>'*)
            email=${ident#*<}
            email=${email%>}
            ;;
    esac
    printf '%s\n' "$name" | grep -Eiq "$AI_NAME_PATTERN" && return 0
    printf '%s\n' "$email" | grep -Eiq "$AI_EMAIL_PATTERN" && return 0
    return 1
}

check_message_file() {
    file=$1
    if message_is_forbidden "$(cat "$file")"; then
        die "ERROR: la política de cualpdf prohíbe trailers de atribución a herramientas de IA."
    fi
    for ident in "${GIT_AUTHOR_NAME:-} <${GIT_AUTHOR_EMAIL:-}>" "${GIT_COMMITTER_NAME:-} <${GIT_COMMITTER_EMAIL:-}>"; do
        [ "$ident" = " <>" ] && continue
        if ident_is_forbidden "$ident"; then
            die "ERROR: la política de cualpdf prohíbe autor o committer de herramientas de IA ($ident)."
        fi
    done
}

check_commit() {
    sha=$1
    if message_is_forbidden "$(git log -1 --format='%B' "$sha")"; then
        die "ERROR: el commit $sha tiene un trailer de atribución a una herramienta de IA."
    fi
    author=$(git log -1 --format='%an <%ae>' "$sha")
    committer=$(git log -1 --format='%cn <%ce>' "$sha")
    if ident_is_forbidden "$author"; then
        die "ERROR: el commit $sha tiene autor de IA ($author)."
    fi
    if ident_is_forbidden "$committer"; then
        die "ERROR: el commit $sha tiene committer de IA ($committer)."
    fi
}

check_commits() {
    if [ "$#" -eq 0 ]; then
        set -- HEAD
    fi
    # Avoid a pipe subshell so a rejected commit fails the whole script.
    shas=$(git rev-list --reverse "$@")
    [ -n "$shas" ] || return 0
    for sha in $shas; do
        check_commit "$sha"
    done
}

usage() {
    die "uso: $0 [--message-file FILE | --commits REV...]"
}

if [ "$#" -eq 0 ]; then
    check_commits HEAD
    exit 0
fi

case "$1" in
    --message-file)
        [ "$#" -eq 2 ] || usage
        check_message_file "$2"
        ;;
    --commits)
        shift
        check_commits "$@"
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage
        ;;
esac
