#ifndef PROJECT_SYNCHRONIZED_OUTPUT_HPP_
#define PROJECT_SYNCHRONIZED_OUTPUT_HPP_

#include <iosfwd>
#include <memory>

namespace lupe {

class ScopedSynchronizedOutput final {
public:
  explicit ScopedSynchronizedOutput(std::ostream &output);
  ~ScopedSynchronizedOutput() noexcept;

  ScopedSynchronizedOutput(const ScopedSynchronizedOutput &) = delete;
  ScopedSynchronizedOutput &
  operator=(const ScopedSynchronizedOutput &) = delete;
  ScopedSynchronizedOutput(ScopedSynchronizedOutput &&) = delete;
  ScopedSynchronizedOutput &operator=(ScopedSynchronizedOutput &&) = delete;

private:
  class Buffer;

  std::ostream &output_;
  std::streambuf *upstream_{nullptr};
  std::unique_ptr<Buffer> buffer_;
};

} // namespace lupe

#endif // PROJECT_SYNCHRONIZED_OUTPUT_HPP_
