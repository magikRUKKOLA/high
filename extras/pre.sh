#!/bin/bash

_get_language() {
    local file="$1"
    local basename="${file##*/}"
    local extension="${basename##*.}"
    
    # If no extension (filename equals extension or no dot)
    if [ "$basename" = "$extension" ] || [ -z "$extension" ]; then
        echo ""
        return
    fi
    
    # Map common extensions to language names for syntax highlighting
    case "$extension" in
        sh|bash|zsh) echo "sh" ;;
        py) echo "python" ;;
        js) echo "javascript" ;;
        ts) echo "typescript" ;;
        rb) echo "ruby" ;;
        go) echo "go" ;;
        rs) echo "rust" ;;
        c|cc|cpp|h|hpp) echo "cpp" ;;
        java) echo "java" ;;
        cs) echo "csharp" ;;
        php) echo "php" ;;
        html|htm) echo "html" ;;
        css) echo "css" ;;
        json) echo "json" ;;
        xml) echo "xml" ;;
        yaml|yml) echo "yaml" ;;
        md) echo "markdown" ;;
        sql) echo "sql" ;;
        *) echo "$extension" ;;
    esac
}

_print_file() {
    local file="$1"
    local lang=$(_get_language "$file")
    
    echo "File: $(realpath "$file")"
    if [ -n "$lang" ]; then
        echo "\`\`\`$lang"
    else
        echo "\`\`\`"
    fi
    cat "$file"
    if [ -s "$file" ]; then
        if [ -n "$(tail -c1 "$file" | tr -d '\n')" ]; then
            echo
        fi
    fi
    echo "\`\`\`"
    echo
}

_show_files() {
    # Handle case with no arguments
    if [ $# -eq 0 ]; then
        find . -maxdepth 2 -type f -exec sh -c '
            for f; do
                basename_f="${f##*/}"
                ext="${basename_f##*.}"
                [ "$basename_f" = "$ext" ] && ext=""
                
                case "$ext" in
                    sh|bash|zsh) lang="sh" ;;
                    py) lang="python" ;;
                    js) lang="javascript" ;;
                    ts) lang="typescript" ;;
                    rb) lang="ruby" ;;
                    go) lang="go" ;;
                    rs) lang="rust" ;;
                    c|cc|cpp|h|hpp) lang="cpp" ;;
                    java) lang="java" ;;
                    cs) lang="csharp" ;;
                    php) lang="php" ;;
                    html|htm) lang="html" ;;
                    css) lang="css" ;;
                    json) lang="json" ;;
                    xml) lang="xml" ;;
                    yaml|yml) lang="yaml" ;;
                    md) lang="markdown" ;;
                    sql) lang="sql" ;;
                    *) lang="$ext" ;;
                esac
                
                echo "File: $(realpath "$f")"
                if [ -n "$lang" ]; then
                    echo "\`\`\`$lang"
                else
                    echo "\`\`\`"
                fi
                cat "$f"
                if [ -s "$f" ]; then
                    if [ -n "$(tail -c1 "$f" | tr -d '\''\n'\'')" ]; then
                        echo
                    fi
                fi
                echo "\`\`\`"
                echo
            done' sh {} +
        return 0
    fi

    local files=()
    local dirs=()
    local patterns=()

    # Classify arguments
    for arg in "$@"; do
        if [ -f "$arg" ]; then
            files+=("$arg")
        elif [ -d "$arg" ]; then
            dirs+=("$arg")
        else
            patterns+=("$arg")
        fi
    done

    # Process direct files
    for file in "${files[@]}"; do
        _print_file "$file"
    done

    # Process directory searches
    if [ ${#dirs[@]} -gt 0 ]; then
        local search_dir="${dirs[0]}"
        local name_pattern="${patterns[0]:-*}"

        find "$search_dir" -maxdepth 2 -type f -name "$name_pattern" -exec sh -c '
            for f; do
                basename_f="${f##*/}"
                ext="${basename_f##*.}"
                [ "$basename_f" = "$ext" ] && ext=""
                
                case "$ext" in
                    sh|bash|zsh) lang="sh" ;;
                    py) lang="python" ;;
                    js) lang="javascript" ;;
                    ts) lang="typescript" ;;
                    rb) lang="ruby" ;;
                    go) lang="go" ;;
                    rs) lang="rust" ;;
                    c|cc|cpp|h|hpp) lang="cpp" ;;
                    java) lang="java" ;;
                    cs) lang="csharp" ;;
                    php) lang="php" ;;
                    html|htm) lang="html" ;;
                    css) lang="css" ;;
                    json) lang="json" ;;
                    xml) lang="xml" ;;
                    yaml|yml) lang="yaml" ;;
                    md) lang="markdown" ;;
                    sql) lang="sql" ;;
                    *) lang="$ext" ;;
                esac
                
                echo "File: $(realpath "$f")"
                if [ -n "$lang" ]; then
                    echo "\`\`\`$lang"
                else
                    echo "\`\`\`"
                fi
                cat "$f"
                if [ -s "$f" ]; then
                    if [ -n "$(tail -c1 "$f" | tr -d '\''\n'\'')" ]; then
                        echo
                    fi
                fi
                echo "\`\`\`"
                echo
            done' sh {} +
    fi

    # Handle pattern-only search (no directories)
    if [ ${#dirs[@]} -eq 0 ] && [ ${#patterns[@]} -gt 0 ]; then
        find . -maxdepth 2 -type f -name "${patterns[0]}" -exec sh -c '
            for f; do
                basename_f="${f##*/}"
                ext="${basename_f##*.}"
                [ "$basename_f" = "$ext" ] && ext=""
                
                case "$ext" in
                    sh|bash|zsh) lang="sh" ;;
                    py) lang="python" ;;
                    js) lang="javascript" ;;
                    ts) lang="typescript" ;;
                    rb) lang="ruby" ;;
                    go) lang="go" ;;
                    rs) lang="rust" ;;
                    c|cc|cpp|h|hpp) lang="cpp" ;;
                    java) lang="java" ;;
                    cs) lang="csharp" ;;
                    php) lang="php" ;;
                    html|htm) lang="html" ;;
                    css) lang="css" ;;
                    json) lang="json" ;;
                    xml) lang="xml" ;;
                    yaml|yml) lang="yaml" ;;
                    md) lang="markdown" ;;
                    sql) lang="sql" ;;
                    *) lang="$ext" ;;
                esac
                
                echo "File: $(realpath "$f")"
                if [ -n "$lang" ]; then
                    echo "\`\`\`$lang"
                else
                    echo "\`\`\`"
                fi
                cat "$f"
                if [ -s "$f" ]; then
                    if [ -n "$(tail -c1 "$f" | tr -d '\''\n'\'')" ]; then
                        echo
                    fi
                fi
                echo "\`\`\`"
                echo
            done' sh {} +
    fi
}

_show_files "$@"
