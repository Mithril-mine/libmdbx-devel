# Active skills (.opencode/skills)

Это **активные копии** навыков, обнаруживаемые opencode через `skill` tool.
Канонические исходники (для редактирования) — в [`skills/`](../skills/).

Правила синхронизации:

- Профили: `skills/profiles/<name>/SKILL.md` → `.opencode/skills/<name>/SKILL.md`
- Общие навыки: `skills/shared/<name>/SKILL.md` → `.opencode/skills/<name>/SKILL.md`
- Суперпауэрсы: `skills/superpowers/<name>/SKILL.md` → `.opencode/skills/<name>/SKILL.md`

При изменении исходника обнови копию и проверь относительные ссылки
(глубина здесь на один уровень меньше: из `.opencode/skills/<name>/`
путь до корня репозитория — `../../../`).