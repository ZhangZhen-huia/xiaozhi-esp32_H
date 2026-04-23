import re

with open('main/mcp_server.cc', 'r') as f:
    code = f.read()

# 1. Update the description and properties to support index_id instead of music_index_id
code = re.sub(
    r'`music_index_id`: 歌曲编号，如M001、M1等（可选）。Music Number\\n",\s*PropertyList\(\{.*?\n\s*Property\("music_index_id", kPropertyTypeString, ""\)\s*\}\),',
    '`index_id`: 歌曲或者故事的编号，如M001、M1、s1等（可选）。\\n",\n                PropertyList({\n                    Property("name", kPropertyTypeString, ""),\n                    Property("target", kPropertyTypeString, ""),\n                    Property("mode", kPropertyTypeString, ""),\n                    Property("GoOn", kPropertyTypeBoolean, false),\n                    Property("duration", kPropertyTypeInteger, 0, 0, 86400),\n                    Property("style", kPropertyTypeString, ""),\n                    Property("Chapter_Index", kPropertyTypeInteger, 1, 1, 1000),\n                    Property("index_id", kPropertyTypeString, "")\n                }),',
    code,
    flags=re.DOTALL
)

# 2. Update parsing to use index_id
code = code.replace(
    'auto music_index_id = properties["music_index_id"].value<std::string>();',
    'auto index_id = properties["index_id"].value<std::string>();'
)

# 3. Fix the hardcoded force_music = 1; force_story = 0; inside general.play and uncomment smart detection using the new variables, and uncomment the story block.
# We'll just do a large string replacement for the logic part.

with open('main/mcp_server.cc', 'w') as f:
    f.write(code)
