#include "Sampler.h"
#include "Registry.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <thread>


struct SamplerDetail
{
	std::chrono::milliseconds interval;
	std::thread worker;

	std::condition_variable start_signal;
	std::mutex start_mutex;

	std::atomic<bool> startable;
	std::atomic<bool> done;

	long ticks;

	SamplerDetail(std::chrono::milliseconds sampling_interval):
		interval(sampling_interval),
		startable(false),
		done(false),
		ticks(0)
	{
		;;
	}
};

Sampler::Sampler(std::chrono::milliseconds interval, const std::vector<std::string> & countersOrAliases, bool continuous_print_flag) :
	m_detail(new SamplerDetail(interval))
{
	// If no counter selected (default), open them all
	const std::vector<std::string> counterNames = countersOrAliases.empty() ? Registry::availableCounters() : countersOrAliases;
	if (counters.empty()) {
		throw std::runtime_error("No counters available on this system.");
	}

	counters.reserve(counterNames.size());

	for (const auto & name: counterNames) {
		PowerDataSourcePtr counter = Registry::openCounter(name);
		if (!counter) {
			throw std::runtime_error("Unknown counter \"" + name + "\"");
		}
		counters.push_back(counter);
	}

	std::function<void()> atick  = [this]{accumulate_tick();};
	std::function<void()> cptick = [this]{continuous_print_tick();};

	m_detail->worker = std::thread([=]{ run(
		continuous_print_flag ? cptick : atick
	); });
}

Sampler::~Sampler()
{
	delete m_detail;
}

long Sampler::ticks() const
{
	return m_detail->ticks;
}

void Sampler::start(std::chrono::milliseconds delay)
{
	std::this_thread::sleep_for(delay);
	m_detail->startable = true;
	m_detail->start_signal.notify_one();
}

Sampler::result_t Sampler::stop(std::chrono::milliseconds delay)
{
	std::this_thread::sleep_for(delay);
	m_detail->done = true;

	// Finish loop in case we didn't start
	start();
	m_detail->worker.join();

	result_t result;
	std::transform(counters.cbegin(), counters.cend(),
		std::back_inserter(result), [](const PowerDataSourcePtr & tdi) { return tdi->accumulator(); });
	return result;
}

void Sampler::run(std::function<void()> tick)
{
	std::unique_lock<std::mutex> lk(m_detail->start_mutex);
	m_detail->start_signal.wait(lk, [this]{ return m_detail->startable.load(); });

	while (!m_detail->done.load()) {
		// FIXME: tiny skid by scheduling + now(). Global start instead?
		auto entry = std::chrono::high_resolution_clock::now();
		tick();
		m_detail->ticks++;
		std::this_thread::sleep_until(entry + m_detail->interval);
	}
}

void Sampler::accumulate_tick()
{
	for (auto & dev: counters) {
		dev->accumulate();
	}
}

void Sampler::continuous_print_tick()
{
	static char buf[255];
	size_t avail = sizeof(buf);
	size_t pos = 0;
	size_t nbytes;
	for (auto & dev: counters) {
		nbytes = dev->read_string(buf + pos, avail);
		pos += nbytes;
		avail -= nbytes;
		buf[pos - 1] = ',';
	}
	buf[pos - 1] = '\0';
	puts(buf);
}
