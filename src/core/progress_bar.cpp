#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#endif

#include "mamba/core/output.hpp"
#include "mamba/core/progress_bar.hpp"
#include "mamba/core/thread_utils.hpp"

namespace mamba
{
    /*****************
     * ProgressProxy *
     *****************/

    ProgressProxy::ProgressProxy(ProgressBar* ptr,
                                 std::size_t idx)
        : p_bar(ptr)
        , m_idx(idx)
    {
    }

    void ProgressProxy::set_full()
    {
        p_bar->set_full();
    }

    void ProgressProxy::set_progress(size_t current, size_t total)
    {
        if (is_sig_interrupted())
        {
            return;
        }
        p_bar->set_progress(current, total);
        Console::instance().print_progress(m_idx);
    }

    void ProgressProxy::elapsed_time_to_stream(std::stringstream& s)
    {
        if (is_sig_interrupted())
        {
            return;
        }
        p_bar->elapsed_time_to_stream(s);
    }

    void ProgressProxy::mark_as_completed(const std::string_view& final_message)
    {
        if (is_sig_interrupted())
        {
            return;
        }
        // mark as completed should print bar or message at FIRST position!
        // then discard
        //p_bar->mark_as_completed();
        Console::instance().deactivate_progress_bar(m_idx, final_message);
    }

    /**********************
     * ProgressBarManager *
     **********************/

    std::unique_ptr<ProgressBarManager> make_progress_bar_manager(ProgressBarMode mode)
    {
        if (mode == ProgressBarMode::multi)
        {
            return std::make_unique<MultiBarManager>();
        }
        return nullptr;
    }

    /*******************
     * MultiBarManager *
     *******************/

    MultiBarManager::MultiBarManager()
        : m_progress_bars()
        , m_active_progress_bars()
        , m_progress_started(false)
    {
    }

    ProgressProxy MultiBarManager::add_progress_bar(const std::string& name)
    {
        std::string prefix = name;
        prefix.resize(PREFIX_LENGTH - 1, ' ');
        prefix += ' ';

        m_progress_bars.push_back(std::make_unique<DefaultProgressBar>(prefix));

        return ProgressProxy(m_progress_bars[m_progress_bars.size() - 1].get(),
                             m_progress_bars.size() - 1);
    }

    void MultiBarManager::print_progress(std::size_t idx)
    {
        std::size_t cursor_up = m_active_progress_bars.size();
        if (m_progress_started && cursor_up > 0)
        {
            std::cout << cursor::up(cursor_up);
        }

        auto it = std::find(m_active_progress_bars.begin(),
                            m_active_progress_bars.end(),
                            m_progress_bars[idx].get());
        if (it == m_active_progress_bars.end())
        {
            m_active_progress_bars.push_back(m_progress_bars[idx].get());
        }

        print_progress();
        m_progress_started = true;
    }

    void MultiBarManager::deactivate_progress_bar(std::size_t idx, const std::string_view& msg)
    {
        if (Context::instance().no_progress_bars
            && !(Context::instance().quiet || Context::instance().json))
        {
            std::cout << m_progress_bars[idx]->prefix() << " " << msg << '\n';
        }

        auto it = std::find(m_active_progress_bars.begin(),
                            m_active_progress_bars.end(),
                            m_progress_bars[idx].get());
        if (it == m_active_progress_bars.end() || Context::instance().quiet
            || Context::instance().json)
        {
            // if no_progress_bars is true, should return here as no progress bars are
            // active
            std::cout << std::flush;
            return;
        }

        m_active_progress_bars.erase(it);
        int ps = m_active_progress_bars.size();
        std::cout << cursor::up(ps + 1) << cursor::erase_line();
        if (msg.empty())
        {
            m_progress_bars[idx]->print();
            std::cout << std::endl;
        }
        else
        {
            std::cout << msg << std::endl;
        }
        print_progress();
    }

    void MultiBarManager::print(const std::string_view& str, bool skip_progress_bars)
    {
        if (m_progress_started && m_active_progress_bars.size())
        {
            const auto& ps = m_active_progress_bars.size();
            std::cout << cursor::up(ps) << cursor::erase_line() << str << std::endl;
            if (!skip_progress_bars)
            {
                print_progress();
            }
        }
        else
        {
            std::cout << str << std::endl;
        }
    }
    
    void MultiBarManager::print_progress()
    {
        for (auto& bar : m_active_progress_bars)
        {
            bar->print();
            std::cout << '\n';
        }
        std::cout << std::flush;
    }

    /************************
     * AggregatedBarManager *
     ************************/

    /***************
     * ProgressBar *
     ***************/

    namespace
    {
        class ProgressScaleWriter
        {
        public:

            ProgressScaleWriter(int bar_width,
                                const std::string& fill,
                                const std::string& lead,
                                const std::string& remainder)
                : m_bar_width(bar_width)
                , m_fill(fill)
                , m_lead(lead)
                , m_remainder(remainder)
            {
            }

            std::ostream& write(std::ostream& os, std::size_t progress) const
            {
                int pos = static_cast<int>(progress * m_bar_width / 100.0);
                for (int i = 0; i < m_bar_width; ++i)
                {
                    if (i < pos)
                    {
                        os << m_fill;
                    }
                    else if (i == pos)
                    {
                        os << m_lead;
                    }
                    else
                    {
                        os << m_remainder;
                    }
                }
                return os;
            }

        private:

            int m_bar_width;
            std::string m_fill;
            std::string m_lead;
            std::string m_remainder;
        };
    }

    ProgressBar::ProgressBar(const std::string& prefix)
        : m_prefix(prefix)
        , m_start_time_saved(false)
    {
    }

    void ProgressBar::set_start()
    {
        m_start_time = std::chrono::high_resolution_clock::now();
        m_start_time_saved = true;
    }


    void ProgressBar::set_postfix(const std::string& postfix_text)
    {
        m_postfix = postfix_text;
    }

    const std::string& ProgressBar::prefix() const
    {
        return m_prefix;
    }

    void ProgressBar::elapsed_time_to_stream(std::stringstream& s)
    {
        if (m_start_time_saved)
        {
            auto now = std::chrono::high_resolution_clock::now();
            m_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start_time);
            s << "(";
            write_duration(s, m_elapsed_ns);
            s << ") ";
        }
        else
        {
            s << "(--:--) ";
        }
    }

    /**********************
     * DefaultProgressBar *
     **********************/
    
    DefaultProgressBar::DefaultProgressBar(const std::string& prefix)
        : ProgressBar(prefix)
        , m_progress(0)
    {
    }

    void DefaultProgressBar::print()
    {
        std::cout << cursor::erase_line(2) << "\r";
        std::cout << m_prefix << "[";

        std::stringstream pf;
        elapsed_time_to_stream(pf);
        pf << m_postfix;
        auto fpf = pf.str();
        int width = get_console_width();
        width = (width == -1)
                    ? 20
                    : (std::min)(static_cast<int>(width - (m_prefix.size() + 4) - fpf.size()), 20);

        if (!m_activate_bob)
        {
            ProgressScaleWriter w{ width, "=", ">", " " };
            w.write(std::cout, m_progress);
        }
        else
        {
            auto pos = static_cast<int>(m_progress * width / 100.0);
            for (int i = 0; i < width; ++i)
            {
                if (i == pos - 1)
                {
                    std::cout << '<';
                }
                else if (i == pos)
                {
                    std::cout << '=';
                }
                else if (i == pos + 1)
                {
                    std::cout << '>';
                }
                else
                {
                    std::cout << ' ';
                }
            }
        }
        std::cout << "] " << fpf;
    }

    void DefaultProgressBar::set_full()
    {
        if (!m_start_time_saved)
        {
            set_start();
        }
        m_activate_bob = false;
        m_progress = 100;
    }

    void DefaultProgressBar::set_progress(size_t current, size_t total)
    {
        if (!m_start_time_saved)
        {
            set_start();
        }

        if (current == SIZE_MAX)
        {
            m_activate_bob = true;
            m_progress += 5;
        }
        else
        {
            size_t p = static_cast<double>(current) / static_cast<double>(total) * 100.;
            m_activate_bob = false;
            m_progress = p;
        }
    }

    /*********************
     * HiddenProgressBar *
     *********************/

    HiddenProgressBar::HiddenProgressBar(const std::string& prefix)
        : ProgressBar(prefix)
    {
    }

    void HiddenProgressBar::print()
    {
        // TODO
    }

    void HiddenProgressBar::set_progress(size_t current, size_t total)
    {
        // TODO
    }
}

