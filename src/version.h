// Copyright (c) 2012-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VERSION_H
#define BITCOIN_VERSION_H

/**
 * network protocol versioning
 */

static const int PROTOCOL_VERSION = 70929;

//! KDD-085 §9.13(h): the version at which a node speaks P2P signing
//! (ptxsignreq/ptxsignresp). A caller MUST NOT send to a peer below this --
//! an older node ignores an unknown command silently (net_processing.cpp
//! "Ignore unknown commands"), giving the caller nothing it can classify.
static const int PTX_SIGNREQ_MIN_PROTO_VERSION = 70929;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT = 70928;
static const int MIN_PEER_PROTO_VERSION_AFTER_ENFORCEMENT = 70928;

//! Version where BIP155 was introduced
static const int MIN_BIP155_PROTOCOL_VERSION = 70923;

//! Version where GMAUTH was introduced
static const int GMAUTH_NODE_VER_VERSION = 70925;

// Make sure that none of the values above collide with
// `ADDRV2_FORMAT`.

#endif // BITCOIN_VERSION_H
