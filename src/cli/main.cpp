// faulttwin-synth: synthetic telemetry generation + validation demonstration.

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include "faulttwin/telemetry/synthetic.hpp"
#include "faulttwin/telemetry/validation.hpp"
#include "options.hpp"
#include "report.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;
constexpr int kExitInternalError = 3;

int run(const faulttwin::cli::Options& options) {
    namespace tel = faulttwin::telemetry;
    const auto samples = tel::generate_synthetic_samples(options.config);
    const tel::Timestamp now = tel::synthetic_reference_now(options.config);

    faulttwin::cli::RunSummary summary;
    for (const auto& sample : samples) {
        const tel::ValidationResult result = tel::validate_sample(sample.raw, now);
        faulttwin::cli::accumulate(summary, sample, result);
        if (!options.quiet) {
            faulttwin::cli::write_sample_line(std::cout, options.format, sample, result);
        }
    }
    faulttwin::cli::write_summary(std::cout, options.format, options, summary);
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "error: failed to write output\n";
        return kExitInternalError;
    }
    return summary.ground_truth_mismatches == 0 ? kExitOk : kExitMismatch;
}

}  // namespace

int main(int argc, char** argv) {
    // Exceptions are not expected here (bad input is reported as values), but
    // std::bad_alloc and friends must not escape main without a diagnostic.
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        const std::string_view program = argc > 0 ? argv[0] : "faulttwin-synth";
        const auto parsed = faulttwin::cli::parse_options(args);

        if (std::holds_alternative<faulttwin::cli::HelpRequested>(parsed)) {
            std::cout << faulttwin::cli::usage_text(program);
            return kExitOk;
        }
        if (const auto* usage = std::get_if<faulttwin::cli::UsageError>(&parsed)) {
            std::cerr << "error: " << usage->message << "\n\n"
                      << faulttwin::cli::usage_text(program);
            return kExitUsage;
        }
        return run(std::get<faulttwin::cli::Options>(parsed));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return kExitInternalError;
    } catch (...) {
        std::cerr << "fatal: unknown exception\n";
        return kExitInternalError;
    }
}
