#!/usr/bin/env bash

# Color codes for pretty output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default input file
INPUT_FILE=${1:-files.txt}
TARGET_DIR=${2:-.}   # Target directory, default current

# Check if input file exists
if [[ ! -f "$INPUT_FILE" ]]; then
    echo -e "${RED}Error: Input file '$INPUT_FILE' not found${NC}"
    exit 1
fi

# Check if target directory exists
if [[ ! -d "$TARGET_DIR" ]]; then
    echo -e "${RED}Error: Target directory '$TARGET_DIR' not found${NC}"
    exit 1
fi

echo -e "${BLUE}=== GGUF File Renamer ===${NC}"
echo -e "${YELLOW}Reading URLs from: $INPUT_FILE${NC}"
echo -e "${YELLOW}Target directory: $TARGET_DIR${NC}"
echo ""

# Build mapping from SHA256 to expected filename
declare -A sha_map
# Array to record expected filenames (for reporting)
expected_files=()

# Function to get the sha256 from the raw info URL
get_sha256_from_url() {
    local url=$1
    # Transform URL to get the raw info link
    info_url="${url/resolve\/main/raw\/main}"
    info_url="${info_url/\?download=true/}"
    # Extract filename from URL (for logging)
    filename=$(basename "$url" | cut -d'?' -f1)

    # Get the reference info
    if ! ref_info=$(curl -sSL "$info_url"); then
        echo -e "  ${RED}Error: Failed to fetch reference info for $filename${NC}" >&2
        return 1
    fi

    # Extract reference sha256
    ref_sha256=$(echo "$ref_info" | grep -oP 'oid sha256:\K[0-9a-f]+')

    if [[ -z "$ref_sha256" ]]; then
        echo -e "  ${RED}Error: Could not extract reference sha256${NC}" >&2
        return 1
    fi

    echo "$ref_sha256"
    return 0
}

# Process each URL in the input file
while IFS= read -r url || [[ -n "$url" ]]; do
    # Skip empty lines
    [[ -z "$url" ]] && continue

    echo -e "${BLUE}Processing URL:${NC} $url"

    # Extract filename from URL
    filename=$(basename "$url" | cut -d'?' -f1)

    # Get the reference sha256
    echo -e "  ${YELLOW}Fetching reference SHA256...${NC}"
    if ! ref_sha256=$(get_sha256_from_url "$url"); then
        continue
    fi

    echo -e "  ${GREEN}Reference SHA256: $ref_sha256${NC}"

    # Check for duplicate sha256
    if [[ -n "${sha_map[$ref_sha256]}" ]]; then
        echo -e "  ${RED}Error: Duplicate SHA256 detected for filename '$filename' and '${sha_map[$ref_sha256]}'${NC}"
        exit 1
    fi

    sha_map[$ref_sha256]="$filename"
    expected_files+=("$filename")

    sleep 1
done < "$INPUT_FILE"

echo -e "${GREEN}Collected ${#expected_files[@]} references.${NC}"

# Now, scan the target directory for files and rename accordingly
echo -e "${BLUE}Scanning target directory for matching files...${NC}"

renamed_count=0
already_named_count=0
not_found_list=()

# Use find to get all files in the target directory (non-recursive) and process them
while IFS= read -r -d $'\0' filepath; do
    filename=$(basename "$filepath")
    echo -e "  ${YELLOW}Checking: $filename${NC}"

    # Compute SHA256 of the local file
    echo -e "    ${YELLOW}Computing SHA256...${NC}"
    local_sha256=$(sha256sum "$filepath" | awk '{print $1}')
    echo -e "    ${GREEN}Computed SHA256: $local_sha256${NC}"

    # Check if this sha256 is in our map
    if [[ -n "${sha_map[$local_sha256]}" ]]; then
        expected_filename="${sha_map[$local_sha256]}"
        # Remove from map so we know it was found
        unset sha_map[$local_sha256]

        # If the current filename is the same as expected, skip
        if [[ "$filename" == "$expected_filename" ]]; then
            echo -e "    ${GREEN}File is already named correctly.${NC}"
            ((already_named_count++))
        else
            # Rename the file
            new_path="$TARGET_DIR/$expected_filename"
            echo -e "    ${YELLOW}Renaming to: $expected_filename${NC}"
            mv -v -- "$filepath" "$new_path"
            ((renamed_count++))
        fi
    else
        echo -e "    ${BLUE}No matching reference SHA256.${NC}"
    fi
done < <(find "$TARGET_DIR" -maxdepth 1 -type f -print0)

# Report on expected files that were not found
if [[ ${#sha_map[@]} -gt 0 ]]; then
    echo -e "${RED}The following expected files were not found:${NC}"
    for sha in "${!sha_map[@]}"; do
        echo -e "  ${RED}- ${sha_map[$sha]} (SHA256: $sha)${NC}"
        not_found_list+=("${sha_map[$sha]}")
    done
fi

# Summary
echo -e "${BLUE}=== Summary ===${NC}"
echo -e "  ${GREEN}Renamed files: $renamed_count${NC}"
echo -e "  ${GREEN}Already correctly named: $already_named_count${NC}"
echo -e "  ${RED}Not found: ${#not_found_list[@]}${NC}"

echo -e "${BLUE}=== Renaming complete ===${NC}"
