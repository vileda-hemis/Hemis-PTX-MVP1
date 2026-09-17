# ptx_client.gd — PTX roll client for Godot. Register as an autoload named PTXClient.
#
# Godot 4.3+. Godot 3.x is NOT supported: typed signals, `await`, and typed Dictionary
# returns do not exist there. There is no 3.x branch.
#
# ★ THE CONTRACT THIS TALKS TO IS NOT DEFINED HERE. It is
#   vileda-hemis/PTX-Kingmaker → docs/PTXPAL_API.md  (public, and authoritative)
# Read it before this file. Anything here that disagrees with it is a bug here.
#
# ★ TOPOLOGY — server-authoritative, and it is the only supported shape. Your backend
# holds the PTXPal key and calls the rail with `X-PTXPal-Key`; this client talks to YOUR
# backend. A Godot export is decompilable: extracting a .pck and recovering GDScript is
# routine, so a key shipped in a client is a public credential attached to a roll
# entitlement someone else paid for. Direct-from-client would need per-install scoped
# tokens with caps and revocation, which do not exist.
# ★★ THERE IS DELIBERATELY NO KEY FIELD ON THIS CLIENT AT ALL, and that is a stronger
# guarantee than a check: you cannot embed what the class cannot hold. An earlier version
# of this header claimed the client "refuses to run in a release export with a key
# embedded" -- it did not, there was no such check and no key field to check, and a stated
# protection with no line producing it is exactly the failure KDD-132 names. The absence
# IS the mechanism; the comment now describes it rather than a check that was never there.
#
# ★ TWO PROPERTIES OF THE RAIL THAT CHANGE HOW YOU WRITE GAME CODE:
#
#   1. ROLLS ARE HELD, MONEY IS NOT. HMS sent to a deposit address belongs to the
#      operator on arrival, in exchange for rolls at a posted rate. An identity holds a
#      COUNT of plays -- "8 raids remaining", never "8 HMS". There is no withdrawal
#      endpoint, no refund path, and no money balance anywhere in the rail. So this
#      client reports `rolls_remaining`, and there is deliberately no balance() call.
#
#   2. THE RAIL OWNS THE ROLL SHAPE. Every roll is count=1, unique=false, no excludes,
#      over a range fixed per app. You do not pass count/low/high: they are not yours to
#      set, and that is a security guarantee rather than a limitation -- the one PTX
#      failure that charges a fee without returning a result is unreachable by customer
#      input. You map the returned value to your own game outcome (see the guide).
extends Node

signal roll_complete(result: Dictionary)
signal roll_failed(code: String, charged: bool, message: String)

## YOUR backend's base URL -- not the rail's. The rail is loopback-only on its own host.
@export var base_url: String = "https://your-backend.example/ptx"
## Offline development: no network, no entitlement spent.
@export var dev_mode: bool = false
## ★ REQUIRED in dev_mode, and there is no sane default. The whole model is that the RANGE
## IS FIXED PER APP -- yours might be 1..20 -- so a stub inventing 1..10000 gives values
## your mapping never sees in production, or worse, values it silently mishandles only in
## dev. Set this to your app's range. Left at 0, every dev roll fails loudly rather than
## returning a plausible number from the wrong interval.
@export var dev_range_high: int = 0
## ★ Settable so you can rehearse EXHAUSTION. Running out is a production outage for your
## game (see the guide's checklist) and it was previously the one path a developer could
## not test locally, because this always answered 999. Set it to 0 and handle it.
@export var dev_rolls_remaining: int = 999
## Rail capacity in dev_mode. Distinct from the above: this is whether the RAIL can serve
## anyone, not whether YOU have plays left. Both can refuse a roll, for different reasons.
@export var dev_available: int = 999
## Non-zero makes dev rolls REPRODUCIBLE; see _dev_roll for why 0 (varying) is the default.
@export var dev_seed: int = 0

# ★ 45 s, not a guessed 5. The sign round's own wall is 30 s and a roll may legitimately
# take it; the rail adds its pre-flight on top. A shorter timeout does not make the roll
# faster, it makes the client lie about a roll that is still running.
const ROLL_TIMEOUT_S: float = 45.0
# ★ Reads are not rolls and do not wait on a signing round: no quorum, no chain, just the
# rail's own SQLite. 15 s is generous for that. The guide publishes both numbers so a
# developer seeing a read time out in 15 s does not read it as the 45 s contract failing.
const READ_TIMEOUT_S: float = 15.0

var _inflight: Dictionary = {}
var _rng := RandomNumberGenerator.new()

func _ready() -> void:
	_rng.randomize()
	if dev_mode and OS.has_feature("release"):
		push_error("PTXClient: dev_mode is on in a RELEASE export. Refusing to start.")
		get_tree().quit(1)
	if dev_seed != 0:
		_rng.seed = dev_seed

## Ask for one roll on behalf of `identity`.
##
## `tag` is the ONLY place your game's semantics live. The rail composes the on-chain
## game_id as  <app>:<tag>:q=<seq>:p=<identity>  and the quorum signs that, so whatever
## you put in `tag` is committed on chain before the result exists -- the difficulty, the
## match, the item table. Keep it short: the whole composed string is capped at 128 bytes.
##
## ★ You do NOT need to make anything unique. The rail assigns a strictly increasing `seq`
## at dequeue and folds it into the signed game_id, so two identical requests in one block
## cannot collide. (Without that they would: the seed is game_id + height + params, so the
## same inputs in one block give the same number, by design, to stop re-rolling.) The
## in-flight guard below is therefore NOT about randomness -- it stops a double-tapped
## button spending two entitlements on one game event.
func roll(identity: String, tag: String = "") -> Dictionary:
	if identity.is_empty():
		return _fail("BAD_REQUEST", false, "identity is required")
	var event_key := "%s|%s" % [identity, tag]
	if _inflight.has(event_key):
		return _fail("DUPLICATE_INFLIGHT", false, "a roll for this event is already in flight")
	if dev_mode:
		return _dev_roll(identity, tag)

	_inflight[event_key] = true
	# request_id is unique per app and is the idempotency key: a replay returns the
	# ORIGINAL result and never spends a second entitlement.
	var body := {"request_id": _request_id(), "identity": identity, "tag": tag}
	var http := HTTPRequest.new()
	add_child(http)
	http.timeout = ROLL_TIMEOUT_S
	var err := http.request(base_url + "/v1/roll",
		PackedStringArray(["Content-Type: application/json"]),
		HTTPClient.METHOD_POST, JSON.stringify(body))
	if err != OK:
		http.queue_free(); _inflight.erase(event_key)
		return _fail("TRANSPORT", false, "request could not be sent (%d)" % err)
	var res: Array = await http.request_completed
	http.queue_free()
	_inflight.erase(event_key)
	return _parse(res)

## How many plays this identity has left. A COUNT, not money.
func rolls_remaining(identity: String) -> int:
	if dev_mode: return dev_rolls_remaining
	var out := await _get("/v1/rolls/" + identity.uri_encode())
	return int(out.get("rolls", 0))

## Whether the rail can serve a roll at all right now.
func available() -> int:
	if dev_mode: return dev_available
	var out := await _get("/v1/availability")
	return int(out.get("rolls_available", 0))

# --- internals --------------------------------------------------------------

func _get(path: String) -> Dictionary:
	var http := HTTPRequest.new()
	add_child(http)
	http.timeout = READ_TIMEOUT_S
	var err := http.request(base_url + path, PackedStringArray([]), HTTPClient.METHOD_GET)
	if err != OK:
		http.queue_free()
		return _fail("TRANSPORT", false, "request could not be sent (%d)" % err)
	var res: Array = await http.request_completed
	http.queue_free()
	return _parse(res)

func _parse(res: Array) -> Dictionary:
	var result_code: int = res[0]
	var http_code: int = res[1]
	var body: PackedByteArray = res[3]
	if result_code == HTTPRequest.RESULT_TIMEOUT:
		# ★ A timeout is NOT safe to retry blindly: the roll may have completed and spent
		# the entitlement. Re-send the SAME request_id, which returns the original result,
		# or read the roll count. Never issue a fresh request_id for the same event.
		return _fail("TIMEOUT", true,
			"no response within %ds -- the roll may have completed. " % int(ROLL_TIMEOUT_S)
			+ "Re-send the same request_id; do not roll again.")
	if result_code != HTTPRequest.RESULT_SUCCESS:
		return _fail("TRANSPORT", false, "transport failure (%d)" % result_code)
	var parsed: Variant = JSON.parse_string(body.get_string_from_utf8())
	if typeof(parsed) != TYPE_DICTIONARY:
		return _fail("BAD_RESPONSE", false, "response was not JSON")
	var obj: Dictionary = parsed
	# 409 with a result is a replay of a completed request: a success, not a failure.
	if obj.has("duplicate") and obj.has("results"):
		roll_complete.emit(obj)
		return obj
	if http_code >= 200 and http_code < 300 and not obj.has("error"):
		roll_complete.emit(obj)
		return obj
	# ★ `charged` refers to YOUR entitlement count, not to the operator's chain fee.
	# Some failures cost the operator a fee on chain and cost you nothing; that is their
	# ledger, not yours, and your backend should surface this field unchanged.
	var charged: bool = bool(obj.get("charged", http_code == 502))
	return _fail(str(obj.get("code", obj.get("error", "UNKNOWN"))), charged,
		str(obj.get("message", obj.get("error", "unspecified error"))))

func _fail(code: String, charged: bool, message: String) -> Dictionary:
	roll_failed.emit(code, charged, message)
	return {"error": code, "charged": charged, "message": message}

func _request_id() -> String:
	return "gd-%d-%d" % [Time.get_unix_time_from_system(), _rng.randi()]

func _dev_roll(identity: String, tag: String) -> Dictionary:
	# Local stub. Not regtest dev_seed -- that path is not live.
	# ★ FAILS LOUDLY rather than guessing a range. A stub that quietly draws 1..10000 for an
	# app whose range is 1..20 produces a mapping that works in dev and breaks in production,
	# which is the worst failure a dev mode can have.
	if dev_range_high < 1:
		return _fail("DEV_RANGE_UNSET", false,
			"dev_mode is on but dev_range_high is 0. Set it to your app's range "
			+ "(the rail fixes the range per app) -- a guessed range hides mapping bugs.")
	var r := RandomNumberGenerator.new()
	# ★ VARIES BY DEFAULT, and that is a decision rather than an oversight. The common dev
	# task is exercising an OUTCOME TABLE -- a loot tier, a hit/miss split -- and a value
	# fixed per (identity, tag) shows exactly one branch of it forever while looking like
	# working code. So at dev_seed 0 each call draws fresh. Set dev_seed non-zero when you
	# want a REPRODUCIBLE run -- a regression test, a bug report -- and it repeats exactly.
	if dev_seed != 0:
		r.seed = hash(identity + tag) + dev_seed
	else:
		r.randomize()
	var obj := {"results": [r.randi_range(1, dev_range_high)], "roll_txid": "", "seq": 0,
		"game_id": "dev:%s:p=%s" % [tag, identity], "dev_mode": true}
	roll_complete.emit(obj)
	return obj
