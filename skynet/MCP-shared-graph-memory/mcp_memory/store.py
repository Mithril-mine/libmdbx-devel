"""Store — персистентная память роя на libmdbx.

Схема (skynet/mcp-memory-design.md):
  records   DEFAULTS                       key=канон.строка → JSON body
  vocab     DEFAULTS                       key=module:{m} | {m}:{topic} → {}
  history   DEFAULTS                       key=_history:{key}:{ts} → JSON
  ids       DEFAULTS                       key=record_key → uint64 id
  id2key    INTEGERKEY                     key=uint64 id → record_key
  inverted  DUPSORT|DUPFIXED|INTEGERDUP    key=term → set uint64 id
  links     DUPSORT|DUPFIXED|INTEGERDUP    key=subj_id\\0pred → set obj_id
  simhash   INTEGERKEY                     key=uint64 id → uint64 hash
  access    INTEGERKEY                     key=uint64 id → 24B (score,last_access,count)
  archive   DEFAULTS                       key=record_key → JSON (gc-миграция)
  meta      DEFAULTS                       key=b"next_id" → uint64

Контракт: одна write-транзакция на операцию; класс ошибок см. errors.py.
"""

from __future__ import annotations

import json
import math
import os
import sys
import time

from . import index as idx
from . import libmdbx as mdbx
from .errors import (
    MemoryError,
    busy_io,
    internal,
    invalid,
    parse,
    size_limit,
)
from .normalize import RECORD_TYPES, Normalizer

ID_LEN = 8
ACCESS_LEN = 24  # float64 score + uint64 last_access + uint64 count

BUSY_RETRIES = 3
GC_HOT = 0.7
GC_WARM = 0.3


def _pack_u64(n: int) -> bytes:
    return int(n).to_bytes(ID_LEN, sys.byteorder, signed=False)


def _unpack_u64(b: bytes) -> int:
    return int.from_bytes(b, sys.byteorder, signed=False)


def _now() -> int:
    return int(time.time())


class Store:
    """Основной объект: открывает env, управляет таблицами."""

    TABLES = {
        "records": mdbx.DB_DEFAULTS,
        "vocab": mdbx.DB_DEFAULTS,
        "history": mdbx.DB_DEFAULTS,
        "ids": mdbx.DB_DEFAULTS,
        "id2key": mdbx.INTEGERKEY,
        "inverted": mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP,
        "links": mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP,
        "simhash": mdbx.INTEGERKEY,
        "access": mdbx.INTEGERKEY,
        "archive": mdbx.DB_DEFAULTS,
        "meta": mdbx.DB_DEFAULTS,
    }

    def __init__(self, path: str, maxdbs: int = 32, max_value_bytes: int = 16 << 20):
        self.path = path
        self.max_value_bytes = max_value_bytes
        self.env = mdbx.Env(path, maxdbs=maxdbs)
        self.norm = Normalizer()
        self._dbi = {}
        # открываем все таблицы один раз (CREATE) в первой write-txn
        with self.env.begin(readonly=False) as txn:
            for name in self.TABLES:
                self._dbi[name] = txn.open_dbi(name, self.TABLES[name] | mdbx.CREATE)
        self._load_vocab()
        self.next_id = self._load_next_id()
        self.last_gc = None

    # --- init -----------------------------------------------------------------
    def _load_vocab(self) -> None:
        with self.env.begin(readonly=True) as txn:
            cur = txn.cursor(self.dbi(txn, "vocab"))
            rc, key, _ = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                text = key.decode("utf-8", "replace")
                if text.startswith("module:"):
                    self.norm.add_module(text[7:])
                elif ":" in text:
                    _m, _, _t = text.partition(":")
                    self.norm.add_topic(_t)
                rc, key, _ = cur.get(mdbx.CURSOR_NEXT)

    def _load_next_id(self) -> int:
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "meta"), b"next_id")
            return _unpack_u64(val) if val is not None else 1

    def dbi(self, txn, name: str) -> int:
        return self._dbi[name]

    # --- helpers --------------------------------------------------------------
    def _check_size(self, key: bytes, value: bytes) -> None:
        klen = self.env.maxkeysize()
        if len(key) > klen:
            raise size_limit(
                "ключ %d байт > лимита движка %d байт" % (len(key), klen),
                "сократите ключ",
            )
        # прикладной лимит значения (движок допускает до ~2 GiB)
        if len(value) > self.max_value_bytes:
            raise size_limit(
                "значение %d байт > лимита %d байт" % (len(value), self.max_value_bytes),
                "сократите запись или разбейте на несколько записей",
            )

    def _begin_write(self):
        """Write-txn с ретраями по BUSY (класс busy-io)."""
        last_err = None
        for attempt in range(BUSY_RETRIES + 1):
            try:
                return self.env.begin(readonly=False)
            except mdbx.LibmdbxError as e:
                if e.rc != mdbx.RC_BUSY:
                    raise
                last_err = e
                if attempt < BUSY_RETRIES:
                    time.sleep(0.05 * (attempt + 1))
        raise busy_io(desc=str(last_err), action="повторите операцию позже")

    # --- records ---------------------------------------------------------------
    def exists(self, key: str) -> bool:
        with self.env.begin(readonly=True) as txn:
            rc, _ = txn.get(self.dbi(txn, "records"), key.encode())
            return rc == mdbx.RC_SUCCESS

    def touch(self, key: str) -> dict:
        """Фиксирует обращение к записи (LRU): увеличивает access_count."""
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            rc, val = txn.get(rec_dbi, key.encode())
            if val is None:
                raise invalid(
                    "запись %r не существует" % key,
                    "сначала сохраните её через safe_store",
                )
            body = json.loads(val.decode())
            self._touch_access(txn, body.get("_id", 0), score=None, count_delta=1)
        return {"touched": key}

    def _bump_access(self, key: str, force: bool = False) -> None:
        """Обновление LRU при чтении (rate-limit 60с между записями)."""
        try:
            with self._begin_write() as txn:
                rec_dbi = self.dbi(txn, "records")
                rc, val = txn.get(rec_dbi, key.encode())
                if val is None:
                    return
                body = json.loads(val.decode())
                acc_dbi = self._dbi["access"]
                idb = _pack_u64(body.get("_id", 0))
                r2, packed = txn.get(acc_dbi, idb)
                if not force and packed and len(packed) == ACCESS_LEN:
                    _, ts, _ = unpack_access(packed)
                    if _now() - ts < 60:
                        return
                self._touch_access(txn, body.get("_id", 0), score=None, count_delta=1)
        except MemoryError:
            pass  # конкурентный сбой LRU-записи не критичен для чтения

    def get_record(self, key: str):
        """Возвращает dict тела записи или None."""
        with self.env.begin(readonly=True) as txn:
            rc, val = txn.get(self.dbi(txn, "records"), key.encode())
            return json.loads(val.decode()) if val is not None else None

    def _body(self, rtype: str, summary: str, importance: float, date: str,
              record_id: int, terms: list) -> bytes:
        body = {
            "type": rtype,
            "summary": summary,
            "importance": float(importance),
            "date": date,
            "_id": record_id,
            "_terms": terms,
        }
        return json.dumps(body, ensure_ascii=False, sort_keys=True).encode("utf-8")

    def safe_store(self, raw_key: str, rtype: str, summary: str, importance: float,
                   date: str = ""):
        """Атомарная операция сохранения с дедупликацией. Одна write-txn."""
        if rtype not in RECORD_TYPES:
            raise invalid(
                "тип %r не из допустимого набора" % rtype,
                "используйте один из: %s" % ", ".join(RECORD_TYPES),
            )
        key = self.norm.normalize(raw_key)
        date = date or time.strftime("%Y-%m-%d", time.gmtime())
        try:
            importance = float(importance)
        except (TypeError, ValueError):
            raise invalid("Важность должна быть числом 0..1", "передайте число") from None
        importance = max(0.0, min(1.0, importance))

        summary = (summary or "").strip()
        if not summary:
            raise invalid("Суть записи пуста", "заполните поле summary")

        terms = idx.tokenize(summary)

        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            rc, old_val = txn.get(rec_dbi, key.encode())
            if rc == mdbx.RC_SUCCESS:
                return self._merge(txn, key, rtype, summary, importance, date,
                                   terms, old_val)
            # новая запись: SimHash-дедуп
            dup = self._find_conflict(txn, summary)
            if dup is not None:
                return {"result": "conflict", "key": dup}

            record_id = self.next_id
            self.next_id += 1
            txn.put(self.dbi(txn, "meta"), b"next_id", _pack_u64(self.next_id))

            self._put_record(txn, key, self._body(rtype, summary, importance,
                                                  date, record_id, terms))
            self._index_add(txn, key, record_id, terms, summary)
            return {"result": "created", "key": key, "id": record_id}

    def _merge(self, txn, key, rtype, summary, importance, date, terms, old_val):
        old = json.loads(old_val.decode())
        old_terms = old.get("_terms", [])
        record_id = old.get("_id", self.next_id)
        # история старой версии
        hist_key = "_history:%s:%d" % (key, _now())
        txn.put(self.dbi(txn, "history"), hist_key.encode(), old_val)
        self._put_record(txn, key, self._body(rtype, summary, importance,
                                              date, record_id, terms))
        self._index_update_terms(txn, key, record_id, old_terms, terms)
        self._index_add_terms(txn, record_id, terms)
        new_hash = idx.simhash(summary)
        txn.put(self.dbi(txn, "simhash"), _pack_u64(record_id), _pack_u64(new_hash))
        self._touch_access(txn, record_id, score=None, count_delta=0)
        return {"result": "merged", "key": key, "id": record_id}

    def _put_record(self, txn, key: str, body: bytes) -> None:
        k = key.encode()
        self._check_size(k, body)
        txn.put(self.dbi(txn, "records"), k, body, mdbx.PUT_UPSERT)

    def _index_add(self, txn, key, record_id, terms, summary) -> None:
        self._index_add_terms(txn, record_id, terms)
        txn.put(self.dbi(txn, "ids"), key.encode(), _pack_u64(record_id))
        txn.put(self.dbi(txn, "id2key"), _pack_u64(record_id), key.encode())
        h = idx.simhash(summary)
        txn.put(self.dbi(txn, "simhash"), _pack_u64(record_id), _pack_u64(h))
        self._touch_access(txn, record_id, score=None, count_delta=0)

    def _index_add_terms(self, txn, record_id, terms) -> None:
        inv_dbi = self.dbi(txn, "inverted")
        with txn.cursor(inv_dbi) as cur:
            idb = _pack_u64(record_id)
            for term in terms:
                cur.put_nodupe(term.encode(), idb)

    def _index_update_terms(self, txn, key, record_id, old_terms, new_terms) -> None:
        """Убирает из inverted термы, которых больше нет в записи."""
        removed = set(old_terms) - set(new_terms)
        if not removed:
            return
        inv_dbi = self.dbi(txn, "inverted")
        idb = _pack_u64(record_id)
        for term in removed:
            txn.delete(inv_dbi, term.encode(), idb)

    def _touch_access(self, txn, record_id, score=None, count_delta=1) -> None:
        acc_dbi = self.dbi(txn, "access")
        packed = _pack_u64(record_id)
        rc, old = txn.get(acc_dbi, packed)
        if old is not None and len(old) == ACCESS_LEN:
            old_score = struct_double(old[0:8])
            old_ts = _unpack_u64(old[8:16])
            old_cnt = _unpack_u64(old[16:24])
        else:
            old_score = score if score is not None else 0.5
            old_ts = _now()
            old_cnt = 0
        new_score = score if score is not None else old_score
        new_ts = _now()
        new_cnt = old_cnt + max(count_delta, 0)
        txn.put(acc_dbi, packed, pack_access(new_score, new_ts, new_cnt))

    def _find_conflict(self, txn, summary: str):
        """SimHash Hamming <= 3 → ключ ближайшей записи (или None)."""
        h = idx.simhash(summary)
        sim_dbi = self.dbi(txn, "simhash")
        id2key_dbi = self.dbi(txn, "id2key")
        with txn.cursor(sim_dbi) as cur:
            rc, idk, val = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                rec_id = _unpack_u64(idk)
                other = _unpack_u64(val)
                if idx.hamming(h, other) <= 3:
                    _, k = txn.get(id2key_dbi, idk)
                    if k is not None:
                        return k.decode("utf-8", "replace")
                rc, idk, val = cur.get(mdbx.CURSOR_NEXT)
        return None

    # --- recall / search / lookup ---------------------------------------------
    def _score_of(self, txn, record_id: int, importance: float, incoming: int) -> float:
        acc_dbi = self._dbi["access"]
        rc, packed = txn.get(acc_dbi, _pack_u64(record_id))
        if packed and len(packed) == ACCESS_LEN:
            _, ts, cnt = unpack_access(packed)
            days = max(0.0, (_now() - ts) / 86400.0)
            recency = math.exp(-0.05 * days)
            access_factor = min(cnt, 10) / 10.0
        else:
            recency = 1.0
            access_factor = 0.0
        link_factor = min(incoming, 5) / 5.0
        return (0.35 * importance + 0.25 * recency + 0.20 * access_factor
                + 0.20 * link_factor)

    def recall(self, pattern: str, limit: int = 5):
        limit = max(1, min(int(limit), 10))
        prefix = pattern[:-1] if pattern.endswith("*") else pattern
        out = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_SET_RANGE, prefix.encode())
                pbytes = prefix.encode()
                while rc == mdbx.RC_SUCCESS:
                    if not key.startswith(pbytes):
                        break
                    body = json.loads(val.decode())
                    record = self._decorate(txn, key.decode(), body)
                    out.append(record)
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        out.sort(key=lambda r: r.get("score", 0.0), reverse=True)
        top = out[:limit]
        for r in top:
            self._bump_access(r["key"])
        return top

    def _decorate(self, txn, key, body):
        incoming = self._incoming_count(txn, body.get("_id"))
        score = self._score_of(txn, body.get("_id", 0), body.get("importance", 0.0), incoming)
        return {
            "key": key,
            "type": body.get("type"),
            "summary": body.get("summary"),
            "importance": body.get("importance"),
            "date": body.get("date"),
            "score": round(score, 4),
            "links_in": incoming,
        }

    def _incoming_count(self, txn, record_id: int) -> int:
        links_dbi = self.dbi(txn, "links")
        prefix = _pack_u64(record_id) + b"\0" + b"back:"
        with txn.cursor(links_dbi) as cur:
            rc, key, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
            n = 0
            while rc == mdbx.RC_SUCCESS:
                if not key.startswith(prefix):
                    break
                n += 1
                rc, key, _ = cur.get(mdbx.CURSOR_NEXT)
            return n

    def lookup(self, term: str, limit: int = 50):
        """Точный поиск по инвертированному индексу."""
        words = idx.tokenize(term)
        if not words:
            return []
        id_sets = []
        with self.env.begin(readonly=True) as txn:
            inv_dbi = self.dbi(txn, "inverted")
            id2key_dbi = self.dbi(txn, "id2key")
            for w in words:
                s = set(self._posting(txn, inv_dbi, w))
                if not s:
                    return []
                id_sets.append(s)
            common = id_sets[0]
            for s in id_sets[1:]:
                common &= s
            keys = []
            for rec_id in sorted(common):
                if len(keys) >= limit:
                    break
                _, k = txn.get(id2key_dbi, _pack_u64(rec_id))
                if k is not None:
                    keys.append(k.decode("utf-8", "replace"))
        return keys

    def _posting(self, txn, inv_dbi, term: str) -> list:
        """Постинг-список терма (сортированный, из INTEGERDUP-набора)."""
        out = []
        with txn.cursor(inv_dbi) as cur:
            rc, _, _ = cur.get(mdbx.CURSOR_SET, term.encode())
            if rc != mdbx.RC_SUCCESS:
                return out
            rc, _, dval = cur.get(mdbx.CURSOR_FIRST_DUP)
            while rc == mdbx.RC_SUCCESS:
                out.append(_unpack_u64(dval))
                rc, _, dval = cur.get(mdbx.CURSOR_NEXT_DUP)
        return out

    def search(self, query: str, limit: int = 5):
        """Поиск по термам с пересечением постинг-списков и ранжированием."""
        limit = max(1, min(int(limit), 10))
        keys = self.lookup(query, limit=1000)
        if not keys:
            return []
        rows = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            for k in keys:
                rc, val = txn.get(rec_dbi, k.encode())
                if val is None:
                    continue
                body = json.loads(val.decode())
                rows.append(self._decorate(txn, k, body))
        rows.sort(key=lambda r: r.get("score", 0.0), reverse=True)
        top = rows[:limit]
        for r in top:
            self._bump_access(r["key"])
        return top

    # --- links -----------------------------------------------------------------
    def link(self, subject: str, predicate: str, object_: str):
        skey = self.norm.normalize(subject)
        okey = self.norm.normalize(object_)
        predicate = predicate.strip().lower()
        if not predicate or ":" in predicate or "\0" in predicate:
            raise invalid("предикат недопустим", "используйте один из: caused-by, "
                          "fixed-by, related-to, supersedes, part-of, depends-on")
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            _, sv = txn.get(rec_dbi, skey.encode())
            _, ov = txn.get(rec_dbi, okey.encode())
            if sv is None:
                raise invalid("субъект %r не существует" % skey,
                              "сначала сохраните запись через safe_store")
            if ov is None:
                raise invalid("объект %r не существует" % okey,
                              "сначала сохраните запись через safe_store")
            sid = json.loads(sv.decode()).get("_id")
            oid = json.loads(ov.decode()).get("_id")
            links_dbi = self.dbi(txn, "links")
            fwd = _pack_u64(sid) + b"\0" + predicate.encode()
            back = _pack_u64(oid) + b"\0" + b"back:" + predicate.encode()
            with txn.cursor(links_dbi) as cur:
                added_f = cur.put_nodupe(fwd, _pack_u64(oid))
                cur.put_nodupe(back, _pack_u64(sid))
            return {"result": "linked" if added_f else "exists"}

    def unlink(self, subject: str, predicate: str, object_: str) -> dict:
        skey = self.norm.normalize(subject)
        okey = self.norm.normalize(object_)
        predicate = predicate.strip().lower()
        with self._begin_write() as txn:
            rec_dbi = self.dbi(txn, "records")
            _, sv = txn.get(rec_dbi, skey.encode())
            _, ov = txn.get(rec_dbi, okey.encode())
            if sv is None or ov is None:
                return {"result": "notfound"}
            sid = json.loads(sv.decode()).get("_id")
            oid = json.loads(ov.decode()).get("_id")
            links_dbi = self.dbi(txn, "links")
            fwd = _pack_u64(sid) + b"\0" + predicate.encode()
            back = _pack_u64(oid) + b"\0" + b"back:" + predicate.encode()
            r1 = txn.delete(links_dbi, fwd, _pack_u64(oid))
            txn.delete(links_dbi, back, _pack_u64(sid))
            return {"result": "unlinked" if r1 else "notfound"}

    def graph(self, key: str, depth: int = 1):
        depth = max(0, min(int(depth), 3))
        root = self.norm.normalize(key)
        with self.env.begin(readonly=True) as txn:
            rc, _ = txn.get(self.dbi(txn, "records"), root.encode())
        if rc != mdbx.RC_SUCCESS:
            raise invalid(
                "запись %r не существует" % root,
                "сначала сохраните её через safe_store",
            )
        nodes = {root}
        edges = []
        visited = set()
        frontier = [root]
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            id2key_dbi = self.dbi(txn, "id2key")
            links_dbi = self.dbi(txn, "links")
            for _ in range(depth + 1):
                nxt = []
                for node in frontier:
                    if node in visited:
                        continue
                    visited.add(node)
                    _, v = txn.get(rec_dbi, node.encode())
                    if v is None:
                        continue
                    rid = json.loads(v.decode()).get("_id")
                    prefix = _pack_u64(rid) + b"\0"
                    with txn.cursor(links_dbi) as cur:
                        rc, lk, lv = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
                        while rc == mdbx.RC_SUCCESS:
                            if not lk.startswith(prefix):
                                break
                            pred = lk[ID_LEN + 1:].decode()
                            if pred.startswith("back:"):
                                # обратная связь: ребро object -> subject
                                obj_id = _unpack_u64(lv)
                                _, kobj = txn.get(id2key_dbi, _pack_u64(obj_id))
                                if kobj is not None:
                                    tgt = kobj.decode()
                                    edges.append({"subject": tgt, "predicate": pred[5:], "object": node})
                                    nxt.append(tgt)
                            else:
                                obj_id = _unpack_u64(lv)
                                _, kobj = txn.get(id2key_dbi, _pack_u64(obj_id))
                                if kobj is not None:
                                    tgt = kobj.decode()
                                    edges.append({"subject": node, "predicate": pred, "object": tgt})
                                    nxt.append(tgt)
                            rc, lk, lv = cur.get(mdbx.CURSOR_NEXT)
                    nxt.extend([])
                frontier = [n for n in nxt if n not in nodes]
                for n in frontier:
                    nodes.add(n)
        seen = set()
        unique = []
        for e in edges:
            sig = (e["subject"], e["predicate"], e["object"])
            if sig not in seen:
                seen.add(sig)
                unique.append(e)
        return {"nodes": sorted(nodes), "edges": unique}

    # --- vocab ------------------------------------------------------------------
    def vocab_add(self, module: str, topic: str = ""):
        module = module.strip().lower()
        with self._begin_write() as txn:
            voc_dbi = self.dbi(txn, "vocab")
            if not topic:
                canon = self.norm.add_module(module)
                key = b"module:" + canon.encode()
                rc, _ = txn.get(voc_dbi, key)
                if rc != mdbx.RC_SUCCESS:
                    txn.put(voc_dbi, key, b"{}")
                    return {"result": "added", "key": "module:%s" % canon}
                return {"result": "exists", "key": "module:%s" % canon}
            canon_m = self.norm.add_module(module)
            # модуль тоже персистится (иначе после перезапуска словарь потерян)
            mkey = b"module:" + canon_m.encode()
            rc, _ = txn.get(voc_dbi, mkey)
            if rc != mdbx.RC_SUCCESS:
                txn.put(voc_dbi, mkey, b"{}")
            canon_t = self.norm.add_topic(topic)
            key = ("%s:%s" % (canon_m, canon_t)).encode()
            rc, _ = txn.get(voc_dbi, key)
            if rc != mdbx.RC_SUCCESS:
                txn.put(voc_dbi, key, b"{}")
                return {"result": "added", "key": "%s:%s" % (canon_m, canon_t)}
            return {"result": "exists", "key": "%s:%s" % (canon_m, canon_t)}

    def vocab_find(self, query: str, n: int = 5):
        modules = self.norm.fuzzy_modules(query, n=n)
        topics = self.norm.fuzzy_topics(query, n=n)
        return {"modules": modules, "topics": topics}

    def vocab_list(self):
        return {"modules": sorted(self.norm.modules), "topics": sorted(self.norm.topics)}

    # --- context dump/restore -----------------------------------------------------
    def dump_context(self, task: str = "", milestone: str = "",
                     keys: list = None, hypotheses: list = None) -> dict:
        ctx = {
            "type": "proc",
            "task": task,
            "milestone": milestone,
            "keys": keys or [],
            "hypotheses": hypotheses or [],
            "events": self.recall("event:", limit=5),
        }
        ts = int(time.time())
        key = "proc:memory:context-snapshot"
        with self._begin_write() as txn:
            record_id = self.next_id
            self.next_id += 1
            ctx["_id"] = record_id
            ctx["_terms"] = []
            body = json.dumps(ctx, ensure_ascii=False, sort_keys=True).encode()
            k = key.encode()
            self._check_size(k, body)
            txn.put(self.dbi(txn, "records"), k, body)
            txn.put(self.dbi(txn, "meta"), b"next_id", _pack_u64(self.next_id))
            txn.put(self.dbi(txn, "ids"), k, _pack_u64(record_id))
            txn.put(self.dbi(txn, "id2key"), _pack_u64(record_id), k)
        return {"context_id": str(ts), "key": key}

    def restore_context(self, context_id: str) -> dict:
        return self.recall("proc:memory:context-snapshot", limit=1)

    # --- gc / stats / purge -------------------------------------------------------
    def _all_records(self):
        """Все записи records в память (корпус мал; избегаем вложенных txn)."""
        out = []
        with self.env.begin(readonly=True) as txn:
            rec_dbi = self.dbi(txn, "records")
            with txn.cursor(rec_dbi) as cur:
                rc, key, val = cur.get(mdbx.CURSOR_FIRST)
                while rc == mdbx.RC_SUCCESS:
                    out.append((key, val))
                    rc, key, val = cur.get(mdbx.CURSOR_NEXT)
        return out

    def gc(self, dry_run: bool = True, archive: bool = False, threshold: float = GC_WARM):
        hot, warm, cold = [], [], []
        ids_to_cold = []
        for key, val in self._all_records():
            body = json.loads(val.decode())
            if body.get("type") == "proc" and "context-snapshot" in key.decode():
                hot.append((key, None))
                continue
            rec_id = body.get("_id", 0)
            with self.env.begin(readonly=True) as txn:
                incoming = self._incoming_count(txn, rec_id)
                score = self._score_of(txn, rec_id, body.get("importance", 0.0), incoming)
            item = (key, score)
            if score > GC_HOT:
                hot.append(item)
            elif score > threshold:
                warm.append(item)
            else:
                cold.append(item)
                ids_to_cold.append((key, rec_id, body))
        stats = {
            "hot": len(hot), "warm": len(warm), "cold": len(cold),
            "dry_run": dry_run, "archive": archive,
            "cold_keys": [k.decode() if isinstance(k, bytes) else k for k, _ in cold],
        }
        if not dry_run and archive and cold:
            moved = 0
            for key, rec_id, body in ids_to_cold:
                with self._begin_write() as txn:
                    arch_dbi = self.dbi(txn, "archive")
                    txn.put(arch_dbi, key, json.dumps(body, ensure_ascii=False).encode())
                    self._delete_record(txn, key.decode(), body)
                    moved += 1
            stats["archived"] = moved
        self.last_gc = _now()
        return stats

    def purge(self, keys: list):
        """Явное удаление записей (и их индексов)."""
        removed, missing = [], []
        for raw in keys:
            key = self.norm.normalize(raw)
            with self._begin_write() as txn:
                rec_dbi = self.dbi(txn, "records")
                rc, val = txn.get(rec_dbi, key.encode())
                if val is None:
                    missing.append(key)
                    continue
                body = json.loads(val.decode())
                self._delete_record(txn, key, body)
                removed.append(key)
        return {"removed": removed, "missing": missing}

    def _delete_record(self, txn, key: str, body: dict) -> None:
        rec_id = body.get("_id")
        terms = body.get("_terms", [])
        rec_dbi = self.dbi(txn, "records")
        ids_dbi = self.dbi(txn, "ids")
        id2key_dbi = self.dbi(txn, "id2key")
        inv_dbi = self.dbi(txn, "inverted")
        sim_dbi = self.dbi(txn, "simhash")
        acc_dbi = self.dbi(txn, "access")
        links_dbi = self.dbi(txn, "links")
        txn.delete(rec_dbi, key.encode())
        txn.delete(ids_dbi, key.encode())
        idb = _pack_u64(rec_id)
        txn.delete(id2key_dbi, idb)
        for term in terms:
            txn.delete(inv_dbi, term.encode(), idb)
        txn.delete(sim_dbi, idb)
        txn.delete(acc_dbi, idb)
        self._delete_links_of(txn, links_dbi, idb, rec_id)

    def _delete_links_of(self, txn, links_dbi, idb, rec_id) -> None:
        # удаляем все рёбра, где субъект = rec_id
        prefix = idb + b"\0"
        with txn.cursor(links_dbi) as cur:
            rc, lk, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
            while rc == mdbx.RC_SUCCESS:
                if not lk.startswith(prefix):
                    break
                cur.delete()
                rc, lk, _ = cur.get(mdbx.CURSOR_SET_RANGE, prefix)
        # удаляем рёбра, где объект = rec_id (обратные/прямые)
        with txn.cursor(links_dbi) as cur:
            rc, lk, lv = cur.get(mdbx.CURSOR_FIRST)
            while rc == mdbx.RC_SUCCESS:
                nxt = cur.get(mdbx.CURSOR_NEXT)
                if _unpack_u64(lv) == rec_id:
                    cur.delete()
                rc, lk, lv = nxt

    def stats(self) -> dict:
        by_type = {}
        total_importance = 0.0
        n = 0
        incoming_top = []
        for key, val in self._all_records():
            body = json.loads(val.decode())
            t = body.get("type", "?")
            by_type[t] = by_type.get(t, 0) + 1
            total_importance += body.get("importance", 0.0)
            n += 1
            rec_id = body.get("_id", 0)
            with self.env.begin(readonly=True) as txn:
                inc = self._incoming_count(txn, rec_id)
            incoming_top.append((key.decode(), inc))
        incoming_top.sort(key=lambda kv: -kv[1])
        return {
            "records": n,
            "by_type": by_type,
            "avg_importance": round(total_importance / n, 3) if n else 0.0,
            "top_linked": incoming_top[:10],
            "vocab_modules": len(self.norm.modules),
            "vocab_topics": len(self.norm.topics),
            "last_gc": self.last_gc,
            "next_id": self.next_id,
        }

    def close(self) -> None:
        self.env.close()

    def __enter__(self) -> "Store":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# --- утилиты упаковки access ----------------------------------------------------
def struct_double(b: bytes) -> float:
    import struct as _struct
    return _struct.unpack("<d", b)[0]


def pack_access(score: float, ts: int, count: int) -> bytes:
    import struct as _struct
    return (_struct.pack("<d", score)
            + _pack_u64(ts) + _pack_u64(count))


def unpack_access(b: bytes):
    import struct as _struct
    assert len(b) == ACCESS_LEN, "bad access length %d" % len(b)
    score = _struct.unpack("<d", b[0:8])[0]
    ts = _unpack_u64(b[8:16])
    cnt = _unpack_u64(b[16:24])
    return score, ts, cnt