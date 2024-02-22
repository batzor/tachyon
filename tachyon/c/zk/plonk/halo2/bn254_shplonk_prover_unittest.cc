#include "tachyon/c/zk/plonk/halo2/bn254_shplonk_prover.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"

#include "tachyon/c/crypto/random/rng.h"
#include "tachyon/c/math/elliptic_curves/bn/bn254/fq_prime_field_traits.h"
#include "tachyon/c/math/elliptic_curves/bn/bn254/fr_prime_field_traits.h"
#include "tachyon/c/math/elliptic_curves/bn/bn254/g1_point_traits.h"
#include "tachyon/c/math/polynomials/constants.h"
#include "tachyon/c/zk/plonk/halo2/bn254_transcript.h"
#include "tachyon/cc/math/elliptic_curves/point_conversions.h"
#include "tachyon/cc/math/finite_fields/prime_field_conversions.h"
#include "tachyon/math/elliptic_curves/bn/bn254/bn254.h"
#include "tachyon/zk/base/commitments/shplonk_extension.h"
#include "tachyon/zk/plonk/halo2/blake2b_transcript.h"
#include "tachyon/zk/plonk/halo2/poseidon_transcript.h"
#include "tachyon/zk/plonk/halo2/prover.h"
#include "tachyon/zk/plonk/halo2/sha256_transcript.h"
#include "tachyon/zk/plonk/halo2/transcript_type.h"

namespace tachyon::zk::plonk::halo2::bn254 {

class SHPlonkProverTest : public testing::TestWithParam<int> {
 public:
  using PCS = SHPlonkExtension<math::bn254::BN254Curve, c::math::kMaxDegree,
                               c::math::kMaxDegree, math::bn254::G1AffinePoint>;

  void SetUp() override {
    k_ = 5;
    s_ = math::bn254::Fr(2);
    const tachyon_bn254_fr& c_s = cc::math::c_cast(s_);
    prover_ = tachyon_halo2_bn254_shplonk_prover_create_from_unsafe_setup(
        GetParam(), k_, &c_s);
  }

  void TearDown() override {
    tachyon_halo2_bn254_shplonk_prover_destroy(prover_);
  }

 protected:
  tachyon_halo2_bn254_shplonk_prover* prover_ = nullptr;
  uint32_t k_;
  math::bn254::Fr s_;
};

INSTANTIATE_TEST_SUITE_P(SHPlonkProverTest, SHPlonkProverTest,
                         testing::Values(TACHYON_HALO2_BLAKE2B_TRANSCRIPT,
                                         TACHYON_HALO2_POSEIDON_TRANSCRIPT,
                                         TACHYON_HALO2_SHA256_TRANSCRIPT));

TEST_P(SHPlonkProverTest, Getters) {
  EXPECT_EQ(tachyon_halo2_bn254_shplonk_prover_get_k(prover_), k_);
  EXPECT_EQ(tachyon_halo2_bn254_shplonk_prover_get_n(prover_), size_t{1} << k_);
  EXPECT_EQ(tachyon_halo2_bn254_shplonk_prover_get_blinder(prover_),
            reinterpret_cast<const tachyon_bn254_blinder*>(
                &(reinterpret_cast<Prover<PCS>*>(prover_)->blinder())));
  EXPECT_EQ(tachyon_halo2_bn254_shplonk_prover_get_domain(prover_),
            reinterpret_cast<const tachyon_bn254_univariate_evaluation_domain*>(
                reinterpret_cast<Prover<PCS>*>(prover_)->domain()));
}

TEST_P(SHPlonkProverTest, Commit) {
  PCS::Domain::DensePoly poly = PCS::Domain::DensePoly::Random(5);
  tachyon_bn254_g1_jacobian* point = tachyon_halo2_bn254_shplonk_prover_commit(
      prover_,
      reinterpret_cast<const tachyon_bn254_univariate_dense_polynomial*>(
          &poly));
  EXPECT_EQ(reinterpret_cast<Prover<PCS>*>(prover_)->Commit(poly).ToJacobian(),
            cc::math::ToJacobianPoint(*point));
}

TEST_P(SHPlonkProverTest, CommitLagrange) {
  PCS::Domain::Evals evals = PCS::Domain::Evals::Random(5);
  tachyon_bn254_g1_jacobian* point =
      tachyon_halo2_bn254_shplonk_prover_commit_lagrange(
          prover_,
          reinterpret_cast<const tachyon_bn254_univariate_evaluations*>(
              &evals));
  EXPECT_EQ(reinterpret_cast<Prover<PCS>*>(prover_)->Commit(evals).ToJacobian(),
            cc::math::ToJacobianPoint(*point));
}

TEST_P(SHPlonkProverTest, SetRng) {
  std::vector<uint8_t> seed = base::CreateVector(
      crypto::XORShiftRNG::kSeedSize,
      []() { return base::Uniform(base::Range<uint8_t>()); });
  tachyon_rng* rng = tachyon_rng_create_from_seed(TACHYON_RNG_XOR_SHIFT,
                                                  seed.data(), seed.size());
  uint8_t state[crypto::XORShiftRNG::kStateSize];
  size_t state_len;
  tachyon_rng_get_state(rng, state, &state_len);
  tachyon_halo2_bn254_shplonk_prover_set_rng_state(prover_, state, state_len);

  auto cpp_rng = std::make_unique<crypto::XORShiftRNG>(
      crypto::XORShiftRNG::FromSeed(seed));
  auto cpp_generator =
      std::make_unique<RandomFieldGenerator<math::bn254::Fr>>(cpp_rng.get());

  EXPECT_EQ(reinterpret_cast<Prover<PCS>*>(prover_)->blinder().Generate(),
            cpp_generator->Generate());

  tachyon_rng_destroy(rng);
}

TEST_P(SHPlonkProverTest, SetTranscript) {
  uint8_t transcript_type = GetParam();

  tachyon_halo2_bn254_transcript_writer* transcript =
      tachyon_halo2_bn254_transcript_writer_create(transcript_type);

  size_t digest_len = 0;
  size_t state_len = 0;
  switch (static_cast<TranscriptType>(transcript_type)) {
    case TranscriptType::kBlake2b: {
      Blake2bWriter<math::bn254::G1AffinePoint>* blake2b =
          reinterpret_cast<Blake2bWriter<math::bn254::G1AffinePoint>*>(
              transcript->extra);
      digest_len = blake2b->GetDigestLen();
      state_len = blake2b->GetStateLen();
      break;
    }
    case TranscriptType::kPoseidon: {
      PoseidonWriter<math::bn254::G1AffinePoint>* poseidon =
          reinterpret_cast<PoseidonWriter<math::bn254::G1AffinePoint>*>(
              transcript->extra);
      digest_len = poseidon->GetDigestLen();
      // NOTE(chokobole): In case of Poseidon transcript,
      // |tachyon_halo2_bn254_transcript_writer_update()| touches an internal
      // member |absorbing_|, so |state_len| has to be updated after this call.
      break;
    }
    case TranscriptType::kSha256: {
      Sha256Writer<math::bn254::G1AffinePoint>* sha256 =
          reinterpret_cast<Sha256Writer<math::bn254::G1AffinePoint>*>(
              transcript->extra);
      digest_len = sha256->GetDigestLen();
      state_len = sha256->GetStateLen();
      break;
    }
  }

  std::vector<uint8_t> data = base::CreateVector(
      digest_len, []() { return base::Uniform(base::Range<uint8_t>()); });
  tachyon_halo2_bn254_transcript_writer_update(transcript, data.data(),
                                               data.size());

  if (static_cast<TranscriptType>(transcript_type) ==
      TranscriptType::kPoseidon) {
    PoseidonWriter<math::bn254::G1AffinePoint>* poseidon =
        reinterpret_cast<PoseidonWriter<math::bn254::G1AffinePoint>*>(
            transcript->extra);
    state_len = poseidon->GetStateLen();
  }

  std::vector<uint8_t> state(state_len);
  tachyon_halo2_bn254_transcript_writer_get_state(transcript, state.data(),
                                                  &state_len);
  tachyon_halo2_bn254_shplonk_prover_set_transcript_state(prover_, state.data(),
                                                          state_len);

  EXPECT_EQ(
      reinterpret_cast<Prover<PCS>*>(prover_)->transcript()->SqueezeChallenge(),
      reinterpret_cast<crypto::TranscriptWriter<math::bn254::G1AffinePoint>*>(
          transcript->extra)
          ->SqueezeChallenge());

  tachyon_halo2_bn254_transcript_writer_destroy(transcript);
}

}  // namespace tachyon::zk::plonk::halo2::bn254