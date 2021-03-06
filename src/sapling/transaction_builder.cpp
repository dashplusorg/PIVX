// Copyright (c) 2018-2020 The Zcash developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "sapling/transaction_builder.h"

#include "script/sign.h"
#include "utilmoneystr.h"
#include "consensus/upgrades.h"

#include <librustzcash.h>

SpendDescriptionInfo::SpendDescriptionInfo(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness) : expsk(expsk), note(note), anchor(anchor), witness(witness)
{
    librustzcash_sapling_generate_r(alpha.begin());
}

Optional<OutputDescription> OutputDescriptionInfo::Build(void* ctx) {
    auto cmu = this->note.cmu();
    if (!cmu) {
        return nullopt;
    }

    libzcash::SaplingNotePlaintext notePlaintext(this->note, this->memo);

    auto res = notePlaintext.encrypt(this->note.pk_d);
    if (!res) {
        return nullopt;
    }
    auto enc = res.get();
    auto encryptor = enc.second;

    libzcash::SaplingPaymentAddress address(this->note.d, this->note.pk_d);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << address;
    std::vector<unsigned char> addressBytes(ss.begin(), ss.end());

    OutputDescription odesc;
    if (!librustzcash_sapling_output_proof(
            ctx,
            encryptor.get_esk().begin(),
            addressBytes.data(),
            this->note.r.begin(),
            this->note.value(),
            odesc.cv.begin(),
            odesc.zkproof.begin())) {
        return nullopt;
    }

    odesc.cmu = *cmu;
    odesc.ephemeralKey = encryptor.get_epk();
    odesc.encCiphertext = enc.first;

    libzcash::SaplingOutgoingPlaintext outPlaintext(this->note.pk_d, encryptor.get_esk());
    odesc.outCiphertext = outPlaintext.encrypt(
        this->ovk,
        odesc.cv,
        odesc.cmu,
        encryptor);

    return odesc;
}

TransactionBuilderResult::TransactionBuilderResult(const CTransaction& tx) : maybeTx(tx) {}

TransactionBuilderResult::TransactionBuilderResult(const std::string& error) : maybeError(error) {}

bool TransactionBuilderResult::IsTx() { return maybeTx != nullopt; }

bool TransactionBuilderResult::IsError() { return maybeError != nullopt; }

CTransaction TransactionBuilderResult::GetTxOrThrow() {
    if (maybeTx) {
        return maybeTx.get();
    } else {
        throw std::runtime_error("Failed to build transaction: " + GetError());
    }
}

Optional<CTransaction> TransactionBuilderResult::GetTx() {
    return maybeTx;
}

std::string TransactionBuilderResult::GetError() {
    if (maybeError) {
        return maybeError.get();
    } else {
        // This can only happen if isTx() is true in which case we should not call getError()
        throw std::runtime_error("getError() was called in TransactionBuilderResult, but the result was not initialized as an error.");
    }
}

// Set default values of new CMutableTransaction based on consensus rules at given height.
CMutableTransaction CreateNewContextualCMutableTransaction(const Consensus::Params& consensusParams, int nHeight)
{
    CMutableTransaction mtx;
    bool isSapling = consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V5_DUMMY);
    mtx.nVersion = isSapling ? CTransaction::SAPLING_VERSION : CTransaction::STANDARD_VERSION;
    return mtx;
}

TransactionBuilder::TransactionBuilder(
    const Consensus::Params& _consensusParams,
    int _nHeight,
    CKeyStore* _keystore) :
    consensusParams(_consensusParams),
    nHeight(_nHeight),
    keystore(_keystore)
{
    mtx = CreateNewContextualCMutableTransaction(consensusParams, nHeight);
}

void TransactionBuilder::AddSaplingSpend(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness)
{
    // Sanity check: cannot add Sapling spend to pre-Sapling transaction
    if (mtx.nVersion < CTransaction::SAPLING_VERSION) {
        throw std::runtime_error("TransactionBuilder cannot add Sapling spend to pre-Sapling transaction");
    }

    // Consistency check: all anchors must equal the first one
    if (spends.size() > 0 && spends[0].anchor != anchor) {
        throw std::runtime_error("Anchor does not match previously-added Sapling spends.");
    }

    spends.emplace_back(expsk, note, anchor, witness);
    mtx.sapData->valueBalance += note.value();
}

void TransactionBuilder::AddSaplingOutput(
    uint256 ovk,
    libzcash::SaplingPaymentAddress to,
    CAmount value,
    std::array<unsigned char, ZC_MEMO_SIZE> memo)
{
    // Sanity check: cannot add Sapling output to pre-Sapling transaction
    if (mtx.nVersion < CTransaction::SAPLING_VERSION) {
        throw std::runtime_error("TransactionBuilder cannot add Sapling output to pre-Sapling transaction");
    }

    auto note = libzcash::SaplingNote(to, value);
    outputs.emplace_back(ovk, note, memo);
    mtx.sapData->valueBalance -= value;
}

void TransactionBuilder::AddTransparentInput(COutPoint utxo, CScript scriptPubKey, CAmount value)
{
    if (keystore == nullptr) {
        throw std::runtime_error("Cannot add transparent inputs to a TransactionBuilder without a keystore");
    }

    mtx.vin.emplace_back(utxo);
    tIns.emplace_back(scriptPubKey, value);
}

void TransactionBuilder::AddTransparentOutput(const CTxDestination& to, CAmount value)
{
    if (!IsValidDestination(to)) {
        throw std::runtime_error("Invalid output address, not a valid taddr.");
    }

    CScript scriptPubKey = GetScriptForDestination(to);
    CTxOut out(value, scriptPubKey);
    mtx.vout.push_back(out);
}

void TransactionBuilder::SetFee(CAmount _fee)
{
    this->fee = _fee;
}

void TransactionBuilder::SendChangeTo(libzcash::SaplingPaymentAddress changeAddr, uint256 ovk)
{
    saplingChangeAddr = std::make_pair(ovk, changeAddr);
    tChangeAddr = nullopt;
}

void TransactionBuilder::SendChangeTo(CTxDestination& changeAddr)
{
    if (!IsValidDestination(changeAddr)) {
        throw std::runtime_error("Invalid change address, not a valid taddr.");
    }

    tChangeAddr = changeAddr;
    saplingChangeAddr = nullopt;
}

TransactionBuilderResult TransactionBuilder::Build()
{
    //
    // Consistency checks
    //

    // Valid change
    CAmount change = mtx.sapData->valueBalance - fee;
    for (auto& tIn : tIns) {
        change += tIn.value;
    }
    for (auto& tOut : mtx.vout) {
        change -= tOut.nValue;
    }
    if (change < 0) {
        return TransactionBuilderResult("Change cannot be negative");
    }

    //
    // Change output
    //

    if (change > 0) {
        // Send change to the specified change address. If no change address
        // was set, send change to the first Sapling address given as input
        // (A t-address can only be used as the change address if explicitly set.)
        if (saplingChangeAddr) {
            AddSaplingOutput(saplingChangeAddr->first, saplingChangeAddr->second, change);
        } else if (tChangeAddr) {
            // tChangeAddr has already been validated.
            AddTransparentOutput(tChangeAddr.value(), change);
        } else if (!spends.empty()) {
            auto fvk = spends[0].expsk.full_viewing_key();
            auto note = spends[0].note;
            libzcash::SaplingPaymentAddress changeAddr(note.d, note.pk_d);
            AddSaplingOutput(fvk.ovk, changeAddr, change);
        } else {
            return TransactionBuilderResult("Could not determine change address");
        }
    }

    //
    // Sapling spends and outputs
    //

    auto ctx = librustzcash_sapling_proving_ctx_init();

    // Create Sapling SpendDescriptions
    for (auto spend : spends) {
        auto cm = spend.note.cmu();
        auto nf = spend.note.nullifier(
            spend.expsk.full_viewing_key(), spend.witness.position());
        if (!cm || !nf) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Spend is invalid");
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << spend.witness.path();
        std::vector<unsigned char> witness(ss.begin(), ss.end());

        SpendDescription sdesc;
        if (!librustzcash_sapling_spend_proof(
                ctx,
                spend.expsk.full_viewing_key().ak.begin(),
                spend.expsk.nsk.begin(),
                spend.note.d.data(),
                spend.note.r.begin(),
                spend.alpha.begin(),
                spend.note.value(),
                spend.anchor.begin(),
                witness.data(),
                sdesc.cv.begin(),
                sdesc.rk.begin(),
                sdesc.zkproof.data())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Spend proof failed");
        }

        sdesc.anchor = spend.anchor;
        sdesc.nullifier = *nf;
        mtx.sapData->vShieldedSpend.push_back(sdesc);
    }

    // Create Sapling OutputDescriptions
    for (auto output : outputs) {
        // Check this out here as well to provide better logging.
        if (!output.note.cmu()) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Output is invalid");
        }

        auto odesc = output.Build(ctx);
        if (!odesc) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Failed to create output description");
        }

        mtx.sapData->vShieldedOutput.push_back(odesc.get());
    }

    //
    // Signatures
    //

    // Empty output script.
    uint256 dataToBeSigned;
    CScript scriptCode;
    try {
        dataToBeSigned = SignatureHash(scriptCode, mtx, NOT_AN_INPUT, SIGHASH_ALL, 0, SIGVERSION_SAPLING);
    } catch (const std::logic_error& ex) {
        librustzcash_sapling_proving_ctx_free(ctx);
        return TransactionBuilderResult("Could not construct signature hash: " + std::string(ex.what()));
    }

    // Create Sapling spendAuth and binding signatures
    for (size_t i = 0; i < spends.size(); i++) {
        librustzcash_sapling_spend_sig(
            spends[i].expsk.ask.begin(),
            spends[i].alpha.begin(),
            dataToBeSigned.begin(),
            mtx.sapData->vShieldedSpend[i].spendAuthSig.data());
    }

    librustzcash_sapling_binding_sig(
        ctx,
        mtx.sapData->valueBalance,
        dataToBeSigned.begin(),
        mtx.sapData->bindingSig.data());

    librustzcash_sapling_proving_ctx_free(ctx);

    // Transparent signatures
    CTransaction txNewConst(mtx);
    for (int nIn = 0; nIn < (int) mtx.vin.size(); nIn++) {
        auto tIn = tIns[nIn];
        SignatureData sigdata;
        bool signSuccess = ProduceSignature(
            TransactionSignatureCreator(
                keystore, &txNewConst, nIn, tIn.value, SIGHASH_ALL),
            tIn.scriptPubKey, sigdata, false);

        if (!signSuccess) {
            return TransactionBuilderResult("Failed to sign transaction");
        } else {
            UpdateTransaction(mtx, nIn, sigdata);
        }
    }

    return TransactionBuilderResult(CTransaction(mtx));
}
