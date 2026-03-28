#!/usr/bin/env bash
# Script to generate API documentation using Doxygen
# Usage: ./generate_docs.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOXYGEN_DIR="${PROJECT_ROOT}/docs/doxygen"
OUTPUT_DIR="${DOXYGEN_DIR}/html"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== MySQL 8.0 Recovery Engine - API Documentation ===${NC}"

# Check if Doxygen is installed
if ! command -v doxygen &> /dev/null; then
    echo -e "${YELLOW}Doxygen not found. Installing...${NC}"
    sudo apt-get update
    sudo apt-get install -y doxygen
fi

# Check if Doxyfile exists
if [ ! -f "${DOXYGEN_DIR}/Doxyfile" ]; then
    echo -e "${RED}Doxyfile not found at ${DOXYGEN_DIR}/Doxyfile${NC}"
    echo -e "${YELLOW}Creating default Doxyfile...${NC}"
    doxygen -g "${DOXYGEN_DIR}/Doxyfile"
fi

# Update Doxyfile with project-specific settings
echo -e "${YELLOW}Updating Doxyfile...${NC}"
sed -i "s/^PROJECT_NAME.*/PROJECT_NAME = \"MySQL 8.0 Recovery Engine\"/" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^OUTPUT_DIRECTORY.*=.*|OUTPUT_DIRECTORY = ${DOXYGEN_DIR}|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^INPUT.*=.*|INPUT = ${PROJECT_ROOT}/include|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_ALL.*=.*|EXTRACT_ALL = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_PRIVATE.*=.*|EXTRACT_PRIVATE = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_STATIC.*=.*|EXTRACT_STATIC = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^SOURCE_BROWSER.*=.*|SOURCE_BROWSER = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^INLINE_SOURCES.*=.*|INLINE_SOURCES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GENERATE_HTML.*=.*|GENERATE_HTML = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GENERATE_LATEX.*=.*|GENERATE_LATEX = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GENERATE_MAN.*=.*|GENERATE_MAN = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GENERATE_RTF.*=.*|GENERATE_RTF = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GENERATE_XML.*=.*|GENERATE_XML = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^CLASS_DIAGRAMS.*=.*|CLASS_DIAGRAMS = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^HIDE_UNDOC_MEMBERS.*=.*|HIDE_UNDOC_MEMBERS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^HIDE_UNDOC_CLASSES.*=.*|HIDE_UNDOC_CLASSES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^REPEAT_BRIEF.*=.*|REPEAT_BRIEF = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^ALWAYS_DETAILED_SEC.*=.*|ALWAYS_DETAILED_SEC = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^FULL_PATH_NAMES.*=.*|FULL_PATH_NAMES = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^STRIP_FROM_PATH.*=.*|STRIP_FROM_PATH = ${PROJECT_ROOT}/|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^STRIP_FROM_INC_PATH.*=.*|STRIP_FROM_INC_PATH = ${PROJECT_ROOT}/include|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_ALL.*=.*|EXTRACT_ALL = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_PRIVATE.*=.*|EXTRACT_PRIVATE = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_STATIC.*=.*|EXTRACT_STATIC = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_LOCAL_CLASSES.*=.*|EXTRACT_LOCAL_CLASSES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_LOCAL_METHODS.*=.*|EXTRACT_LOCAL_METHODS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTRACT_ANON_NSPACES.*=.*|EXTRACT_ANON_NSPACES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^HIDE_FRIEND_COMPOUNDS.*=.*|HIDE_FRIEND_COMPOUNDS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^HIDE_IN_BODY_DOCS.*=.*|HIDE_IN_BODY_DOCS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^INTERNAL_DOCS.*=.*|INTERNAL_DOCS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^CASE_SENSE_NAMES.*=.*|CASE_SENSE_NAMES = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^HIDE_SCOPE_NAMES.*=.*|HIDE_SCOPE_NAMES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^SHOW_INCLUDE_FILES.*=.*|SHOW_INCLUDE_FILES = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^SHOW_NAMESPACES.*=.*|SHOW_NAMESPACES = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^FILE_VERSION_FILTER.*=.*|FILE_VERSION_FILTER = |g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^LAYOUT_FILE.*=.*|LAYOUT_FILE = |g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^EXTENSION_MAPPING.*=.*|EXTENSION_MAPPING = |g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^MARKDOWN_SUPPORT.*=.*|MARKDOWN_SUPPORT = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^TOC_INCLUDE_HEADINGS.*=.*|TOC_INCLUDE_HEADINGS = 5|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^AUTOLINK_SUPPORT.*=.*|AUTOLINK_SUPPORT = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^BUILTIN_STL_SUPPORT.*=.*|BUILTIN_STL_SUPPORT = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^CPP_CLI_SUPPORT.*=.*|CPP_CLI_SUPPORT = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^SIP_SUPPORT.*=.*|SIP_SUPPORT = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^IDL_PROPERTY_SUPPORT.*=.*|IDL_PROPERTY_SUPPORT = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^DISTRIBUTE_GROUP_DOC.*=.*|DISTRIBUTE_GROUP_DOC = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^GROUP_NESTED_COMPOUNDS.*=.*|GROUP_NESTED_COMPOUNDS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^SUBGROUPING.*=.*|SUBGROUPING = YES|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^INLINE_GROUPED_CLASSES.*=.*|INLINE_GROUPED_CLASSES = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^INLINE_SIMPLE_STRUCTS.*=.*|INLINE_SIMPLE_STRUCTS = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^TYPEDEF_HIDES_STRUCT.*=.*|TYPEDEF_HIDES_STRUCT = NO|g" "${DOXYGEN_DIR}/Doxyfile"
sed -i "s|^LOOKUP_CACHE_SIZE.*=.*|LOOKUP_CACHE_SIZE = 0|g" "${DOXYGEN_DIR}/Doxyfile"

# Generate documentation
echo -e "${YELLOW}Generating documentation...${NC}"
cd "${DOXYGEN_DIR}"
doxygen Doxyfile

# Check if documentation was generated
if [ -f "${OUTPUT_DIR}/index.html" ]; then
    echo -e "${GREEN}=== Documentation Generated Successfully ===${NC}"
    echo -e "${GREEN}HTML Documentation: ${OUTPUT_DIR}/index.html${NC}"
    
    # Show documentation size
    DOCSIZE=$(du -sh "${OUTPUT_DIR}" | cut -f1)
    echo -e "${GREEN}Documentation Size: ${DOCSIZE}${NC}"
    
    # Show number of files
    NUMFILES=$(find "${OUTPUT_DIR}" -type f | wc -l)
    echo -e "${GREEN}Number of Files: ${NUMFILES}${NC}"
    
    # Open documentation in browser (if running locally)
    if [ -n "$DISPLAY" ] && command -v xdg-open &> /dev/null; then
        echo -e "${YELLOW}Opening documentation in browser...${NC}"
        xdg-open "${OUTPUT_DIR}/index.html" 2>/dev/null || true
    fi
else
    echo -e "${RED}Documentation generation failed${NC}"
    exit 1
fi

echo -e "${GREEN}Done!${NC}"
