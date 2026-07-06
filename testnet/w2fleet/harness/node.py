"""Thin JSON-RPC wrapper around a single hemisd node."""

import json
import time
import requests


class RPCError(Exception):
    def __init__(self, code, message):
        super().__init__(f"RPC error {code}: {message}")
        self.code = code
        self.message = message


class Node:
    def __init__(self, name: str, host: str, port: int, user: str, password: str):
        self.name = name
        self.host = host
        self.port = port
        self._auth = (user, password)
        self._url = f"http://{host}:{port}/"
        self._id = 0

    def call(self, method: str, *params, timeout: int = 15):
        self._id += 1
        payload = {"jsonrpc": "1.0", "id": self._id, "method": method, "params": list(params)}
        resp = requests.post(self._url, json=payload, auth=self._auth, timeout=timeout)
        data = resp.json()
        if data.get("error"):
            raise RPCError(data["error"]["code"], data["error"]["message"])
        return data["result"]

    # ── convenience methods ────────────────────────────────────────────────

    def getblockcount(self) -> int:
        return self.call("getblockcount")

    def getbestblockhash(self) -> str:
        return self.call("getbestblockhash")

    def getbalance(self) -> float:
        return self.call("getbalance")

    def getnewaddress(self) -> str:
        return self.call("getnewaddress")

    def generatetoaddress(self, n: int, addr: str):
        return self.call("generatetoaddress", n, addr)

    def sendmany(self, account: str, amounts: dict) -> str:
        return self.call("sendmany", account, amounts)

    def sendtoaddress(self, addr: str, amount: float) -> str:
        return self.call("sendtoaddress", addr, amount)

    def listunspent(self, minconf: int = 1) -> list:
        return self.call("listunspent", minconf)

    def getblock(self, blockhash: str, verbose: int = 1) -> dict:
        return self.call("getblock", blockhash, verbose)

    def getblockhash(self, height: int) -> str:
        return self.call("getblockhash", height)

    def getrawtransaction(self, txid: str, verbose: bool = True) -> dict:
        return self.call("getrawtransaction", txid, verbose)

    def getmempoolinfo(self) -> dict:
        return self.call("getmempoolinfo")

    def ptx_roll(self, count: int, low: int, high: int,
                 game_id: str = "test", salt: str = "00aabbcc") -> dict:
        return self.call("ptx_roll", count, low, high, False, [], game_id, salt)

    def ptx_lottery_status(self) -> dict:
        return self.call("ptx_lottery_status")

    def ptx_pose_status(self) -> list:
        return self.call("ptx_pose_status")

    def ptx_gm_pose(self, node_id: str) -> dict:
        return self.call("ptx_gm_pose", node_id)

    def ptx_wallet_lottery_status(self) -> dict:
        return self.call("ptx_wallet_lottery_status")

    def ptx_lottery_history(self) -> list:
        return self.call("ptx_lottery_history")

    def ptx_wallet_operated_gms(self) -> list:
        return self.call("ptx_wallet_operated_gms")

    def protx_register_fund(self, collateral_addr: str, ip_port: str,
                            owner_addr: str, operator_pubkey: str,
                            voting_addr: str, payout_addr: str,
                            operator_reward: int = 0,
                            operator_payout_addr: str = "",
                            ptx_payment_addr: str = "",
                            ptx_node_id: str = "") -> dict:
        return self.call("protx_register_fund",
                         collateral_addr, ip_port, owner_addr, operator_pubkey,
                         voting_addr, payout_addr, operator_reward,
                         operator_payout_addr, ptx_payment_addr, ptx_node_id)

    def protx_list(self, detailed: bool = True, wallet_only: bool = False,
                   valid_only: bool = False) -> list:
        return self.call("protx_list", detailed, wallet_only, valid_only)

    def getdeterministicgmlist(self) -> dict:
        return self.call("getdeterministicgmlist")

    def bls_generate(self) -> dict:
        return self.call("generateblskeypair")

    def wait_for_height(self, target: int, timeout: int = 300, poll: float = 2.0) -> int:
        deadline = time.time() + timeout
        while time.time() < deadline:
            h = self.getblockcount()
            if h >= target:
                return h
            time.sleep(poll)
        raise TimeoutError(
            f"{self.name}: timed out waiting for height {target} "
            f"(stuck at {self.getblockcount()}) after {timeout}s"
        )

    def wait_for_condition(self, pred, desc: str, timeout: int = 300, poll: float = 2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            result = pred()
            if result:
                return result
            time.sleep(poll)
        raise TimeoutError(f"{self.name}: timed out waiting for '{desc}' after {timeout}s")

    def is_rpc_ready(self) -> bool:
        try:
            self.getblockcount()
            return True
        except Exception:
            return False
