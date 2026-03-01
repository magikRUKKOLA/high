#!/bin/bash
INSTANCE_ID=$$
TEMP_FILE="/tmp/codeblocks_${INSTANCE_ID}.tsv"
CONTENT_FILE="/tmp/codeblocks_content_${INSTANCE_ID}"

cleanup() { rm -f "${TEMP_FILE}" "${CONTENT_FILE}"*; }
trap cleanup EXIT

TERM_HEIGHT=$(tput lines)
PREVIEW_HEIGHT=$((TERM_HEIGHT / 2))

HAS_BAT=$(command -v batcat)

awk '
  BEGIN { 
    block_id=0; 
    print "BLOCK_ID\tSTART_LINE\tLINES_COUNT\tLANGUAGE\tPREVIEW" > "'${TEMP_FILE}'"
    in_block=0
    current_block=""
    lang="text"
  }
  
  # Start of code block
  /^```/ && !in_block {
    # Save any preceding text block
    if (current_block != "") {
      save_block()
    }
    
    # Start new code block
    in_block=1
    lang=$0
    sub(/^```/, "", lang)
    lang=gensub(/([^[:space:]]+).*/,"\\1",1,lang)
    if (lang ~ /^[[:space:]]*$/) lang="text"
    current_block=""
    next
  }
  
  # End of code block
  /^```/ && in_block {
    # Save the code block
    save_block()
    in_block=0
    lang="text"
    current_block=""
    next
  }
  
  # Any other line - add to current block
  {
    current_block = current_block == "" ? $0 : current_block "\n" $0
  }
  
  END {
    # Save any remaining content
    if (current_block != "") {
      save_block()
    }
  }
  
  function save_block() {
    block_id++
    line_count = count_lines(current_block)
    preview = current_block
    sub(/\n.*/, "", preview)
    if (length(preview) > 60) preview = substr(preview, 1, 57) "..."
    
    printf "%s", current_block > ("'${CONTENT_FILE}'_" block_id)
    print block_id "\t" (block_id == 1 ? 1 : prev_end + 1) "\t" line_count "\t" lang "\t" preview >> "'${TEMP_FILE}'"
    prev_end = (block_id == 1 ? 1 : prev_end + 1) + line_count - 1
  }
  
  function count_lines(str) {
    n = gsub(/\n/, "", str)
    return n + 1
  }
' "$@"

[ ! -s "${TEMP_FILE}" ] && exit 1

selected=$(awk -F'\t' '
  NR==1 {next}
  { lines[NR]=$0; last=NR }
  END {
    # Process lines in reverse order
    for (i=last; i>=2; i--) {
      $0=lines[i]
      printf "%-3d | %-4d | %-3d | \033[1;34m%-8s\033[0m | %s\n",$1,$2,$3,$4,$5
    }
  }' "${TEMP_FILE}" | fzf --ansi --height=100% \
    --preview='awk -F"\t" -v id={1} '\''NR==1{next}
      $1==id {
        printf "\033[1mBlock %d | Line %d | %d lines | %s\033[0m\n\n",$1,$2,$3,$4
        if ("'$HAS_BAT'" != "") {
          system("batcat --color=always --style=numbers --language=" $4 " \"'${CONTENT_FILE}'_" $1 "\" 2>/dev/null || batcat --color=always --style=numbers \"'${CONTENT_FILE}'_" $1 "\"")
        } else {
          system("cat \"'${CONTENT_FILE}'_" $1 "\"")
        }
        exit
      }'\'' "'${TEMP_FILE}'"' \
    --preview-window="top:${PREVIEW_HEIGHT}:wrap" --header-lines=0 --select-1)

[ -n "$selected" ] && {
  block_id=$(echo "$selected" | awk '{print $1}')
  content_file="${CONTENT_FILE}_${block_id}"
  actual_lines=$(wc -l <"${content_file}")
  [ "$(tail -c1 "${content_file}" | xxd -p)" != "0a" ] && actual_lines=$((actual_lines+1))
  if [ ! -t 1 ]; then cat "${content_file}"; 
  else
    echo "" >&2
    xclip -selection clipboard <"${content_file}" &&
    printf "\033[1;32m✓\033[0m Copied block \033[1;36m%d\033[0m (\033[1;33m%s\033[0m, \033[1;35m%d lines\033[0m) to clipboard\n" \
    "${block_id}" "$(awk -F'\t' -v id="${block_id}" '$1==id{print $4}' "${TEMP_FILE}")" "${actual_lines}" >&2 
  fi
}
