# skill-tree — рабочая копия ветки для навыков (workflow)

> `/sourcecraft/workspace/skill-tree` — git worktree от bare-репозитория
> `local-origin`, на собственной ветке `skill-tree`, отслеживающей `devel`
> (после вливания devel→master — `master`). Назначение: стабильный файловый
> путь для активных навыков, на который указывают глобальные симлинки
> `~/.config/sourcecraft/opencode/skills/*`.

## Зачем нужен

opencode загружает навыки из `.opencode/skills/**/SKILL.md` (проектный) и из
глобального `~/.config/sourcecraft/opencode/skills/*` (симлинки на каталоги).
Чтобы симлинки не «били» по nook-pool-* (которые ротируются между ветками),
нужно стабильное дерево — `skill-tree`.

## Привязка к ветке

Worktree не может чекаутить саму ветку `devel`/`master` (в bare-репозитории она
занята HEAD). Поэтому у skill-tree **собственная ветка `skill-tree`**, которую
обновляют из целевой ветки:

- Сейчас: базируется на `origin/devel`. Пока `feature/skills-profiles` не
  смержена в devel, worktree временно стоит на ней (иначе `.opencode/skills`
  ещё нет в devel и глобальные симлинки были бы битыми). После merge —
  вернуть на `origin/devel`:
  ```sh
  cd /sourcecraft/workspace/skill-tree
  git checkout skill-tree
  git fetch origin
  git reset --hard origin/devel
  ```
- **После вливания devel → master** переключить на master:
  ```sh
  cd /sourcecraft/workspace/skill-tree
  git checkout skill-tree
  git fetch origin
  git reset --hard origin/master
  ```

## Обновление skill-tree после изменений devel/master

```sh
cd /sourcecraft/workspace/skill-tree
git fetch origin
git reset --hard origin/<devel|master>
```

Обновлять нужно ПОСЛЕ каждого merge в devel/master (или по мере необходимости —
перед проверкой навыков).

## Синхронизация с workflow master и перезапуском сессий

1. **Обновление навыков** (`skills/*` или `.opencode/skills/*`) в devel/master:
   - после merge в целевую ветку обновить `skill-tree` (reset --hard);
   - **перезапустить активные opencode-сессии**, которые используют эти навыки:
     навыки загружаются при старте сессии (`Skill.available()` фиксирует список
     на момент init), поэтому текущие сессии не увидят изменения до рестарта.
2. **Напоминание**: при любом коммите, меняющем `skills/` или `.opencode/skills/`,
   в REPORT/PR указывай флаг `SKILLS-UPDATED` + «restart sessions required».
   Координатор выполняет перезапуск канонических сессий (`ses_*` из
   `.skynet/sessions.json`) в assigned nook.
3. **Переключение ветки skill-tree** (devel → master) выполняется координатором
   после вливания; симлинки при этом НЕ меняются (ведут на
   `/sourcecraft/workspace/skill-tree/.opencode/skills/*` — путь стабилен).

## Проверка активации навыков

```sh
# после перезапуска сессии в skill-tree
# список доступных навыков виден в системном промпте (Available Skills)
# точечная проверка одного навыка: skill <name> → содержимое SKILL.md
```

## Порядок действий при обновлении навыков (checklist)

- [ ] Изменены исходники в `skills/` (канон).
- [ ] Синхронизированы активные копии в `.opencode/skills/` (глубина ссылок!
      там путь до корня `../../../`, а не `../../..` как в `skills/profiles/*/`).
- [ ] `skill-tree` обновлён (`git fetch origin && git reset --hard origin/<ветка>`).
- [ ] Сессии перезапущены; `Skill.available()` показывает новые/изменённые.
- [ ] В отчёте указан `SKILLS-UPDATED`.