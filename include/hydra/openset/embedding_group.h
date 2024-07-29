#pragma once
#include <Eigen/Dense>
#include <memory>
#include <string>
#include <vector>

namespace hydra {

struct EmbeddingDistance;

struct EmbeddingGroup {
  using Ptr = std::shared_ptr<EmbeddingGroup>;
  struct ScoreResult {
    float score = std::numeric_limits<float>::lowest();
    size_t index = 0;
  };

  virtual ~EmbeddingGroup();

  bool empty() const;
  operator bool() const { return !empty(); }
  size_t size() const { return embeddings.size(); }

  Eigen::VectorXf getDistances(const EmbeddingDistance& dist,
                               const Eigen::VectorXf& embedding) const;

  Eigen::VectorXf getScores(const EmbeddingDistance& dist,
                            const Eigen::VectorXf& embedding) const;

  ScoreResult getBestScore(const EmbeddingDistance& dist,
                           const Eigen::VectorXf& embedding) const;

  std::vector<Eigen::VectorXf> embeddings;
  std::vector<std::string> names;
};

}  // namespace hydra
