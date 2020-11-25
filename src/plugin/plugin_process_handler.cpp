// system headers
#include <tuple>
#include <thread>
#include <utility>

// local headers
#include <linuxdeploy/plugin/plugin_process_handler.h>
#include <linuxdeploy/subprocess/process.h>
#include <linuxdeploy/util/util.h>
#include <linuxdeploy/core/log.h>
#include <linuxdeploy/subprocess/pipe_reader.h>

namespace bf = boost::filesystem;

namespace linuxdeploy {
    namespace plugin {
        using namespace core::log;

        plugin_process_handler::plugin_process_handler(std::string name, bf::path path) : name_(std::move(name)),
                                                                                          path_(std::move(path)) {}

        int plugin_process_handler::run(const bf::path& appDir) const {
            // prepare arguments and environment variables
            const std::initializer_list<std::string> args = {path_.string(), "--appdir", appDir.string()};

            subprocess::subprocess_env_map_t environmentVariables{};

            // add $LINUXDEPLOY, which points to the current binary
            // we do not need to pass $APPIMAGE or alike, since while linuxdeploy is running, the path in the
            // temporary mountpoint of its AppImage will be valid anyway
            environmentVariables["LINUXDEPLOY"] = linuxdeploy::util::getOwnExecutablePath();

            linuxdeploy::subprocess::process proc{args, environmentVariables};

            // we want to insert a custom log prefix whenever a CR or LF is written into either buffer
            // like in subprocess's check_output, we use pipe readers to read from the subprocess's stdout/stderr pipes
            // however, we just dump everything we receive directly in the log, using our ĺogging framework
            // we store an ldLog instance per stream so we can just send all data into those, which allows us to get away
            // with relatively small buffers (we don't have to cache complete lines or alike)
            class pipe_to_be_logged {
            public:
                pipe_reader reader_;
                std::string stream_name_;
                ldLog log_;
                bool print_prefix_in_next_iteration_;

                pipe_to_be_logged(int pipe_fd, std::string stream_name) : reader_(pipe_fd),
                                                                          stream_name_(std::move(stream_name)),
                                                                          log_(),
                                                                          print_prefix_in_next_iteration_(true) {}
            };

            std::array<pipe_to_be_logged, 2> pipes_to_be_logged{
                pipe_to_be_logged(proc.stdout_fd(), "stdout"),
                pipe_to_be_logged(proc.stderr_fd(), "stderr"),
            };

            for (;;) {
                for (auto& pipe_to_be_logged : pipes_to_be_logged) {
                    const auto log_prefix = "[" + name_ + "/" + pipe_to_be_logged.stream_name_ + "] ";

                    // since we have our own ldLog instance for every pipe, we can get away with this rather small read buffer
                    subprocess::subprocess_result_buffer_t intermediate_buffer(4096);

                    // (try to) read from pipe
                    const auto bytes_read = pipe_to_be_logged.reader_.read(intermediate_buffer);

                    // no action required in case we have not read anything from the pipe
                    if (bytes_read <= 0) {
                        continue;
                    }

                    // we just trim the buffer to the bytes we read (makes the code below easier)
                    intermediate_buffer.resize(bytes_read);

                    // all we have to do now is to look for CR or LF, send everything up to that location into the ldLog instance,
                    // write our prefix and then repeat
                    for (auto it = intermediate_buffer.begin(); it != intermediate_buffer.end(); ++it) {
                        if (pipe_to_be_logged.print_prefix_in_next_iteration_) {
                            pipe_to_be_logged.log_ << log_prefix;
                        }

                        const auto next_lf = std::find(it, intermediate_buffer.end(), '\n');
                        const auto next_cr = std::find(it, intermediate_buffer.end(), '\r');

                        // we don't care which one goes first -- we pick the closest one, write everything up to it into our ldLog,
                        // then print our prefix and repeat that until there's nothing left in our buffer
                        auto next_control_char = std::min({next_lf, next_cr});

                        // if there is a control char, we remember this for the next iteration, where we print our
                        // log prefix
                        // in any case, we can write the remaining buffer contents into the ldLog object
                        pipe_to_be_logged.print_prefix_in_next_iteration_ = (next_control_char !=
                                                                             intermediate_buffer.end());

                        const auto distance_from_begin_to_it = std::distance(intermediate_buffer.begin(), it);
                        auto distance_from_it_to_next_cc = std::distance(it, next_control_char);

                        if (pipe_to_be_logged.print_prefix_in_next_iteration_) {
                            distance_from_it_to_next_cc++;
                        }

                        // need to make sure we include the control char in the write
                        pipe_to_be_logged.log_.write(
                        intermediate_buffer.data() + distance_from_begin_to_it,
                            distance_from_it_to_next_cc
                        );

                        it = next_control_char;

                        // TODO: should not be necessary, should be fixed in for loop
                        if (!pipe_to_be_logged.print_prefix_in_next_iteration_) {
                            break;
                        }
                    }
                }

                // do-while might be a little more elegant, but we can save this one unnecessary sleep, so...
                if (proc.is_running()) {
                    // reduce load on CPU
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    break;
                }
            }

            return proc.close();
        }

    }
}
