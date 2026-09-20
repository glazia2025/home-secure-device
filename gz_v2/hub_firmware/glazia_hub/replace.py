import re
import sys

file_path = '/home/xio/Desktop/glazia_work/glazia-prototype/gz_temp/hub_firmware/glazia_hub/main/display.c'

with open(file_path, 'r') as f:
    content = f.read()

original_content = content

content = re.sub(
    r'if\s*\(\s*!\s*lvgl_port_lock\s*\(\s*(.*?)\s*\)\s*\)\s*return\s*;',
    r'if (xSemaphoreTake(s_lvgl_mux, \1) != pdTRUE) return;',
    content
)

content = re.sub(
    r'if\s*\(\s*lvgl_port_lock\s*\(\s*(.*?)\s*\)\s*\)\s*\{',
    r'if (xSemaphoreTake(s_lvgl_mux, \1) == pdTRUE) {',
    content
)

content = re.sub(
    r'lvgl_port_unlock\s*\(\s*\)\s*;',
    r'xSemaphoreGive(s_lvgl_mux);',
    content
)

if content != original_content:
    with open(file_path, 'w') as f:
        f.write(content)
    print("Replacements made.")
else:
    print("No changes needed or matched.")
