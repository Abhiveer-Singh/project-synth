/*
 * Zephyr real-time audio effects processor
 * Target: Nucleo-F446RE (STM32F446RE)
 *
 * Signal path (Path A: internal ADC/DAC, mono, no external codec):
 *   Audio in (PA0 / ADC1_IN0) -> delay/echo DSP -> Audio out (PA4 / DAC_OUT1)
 *
 * Threads:
 *   audio_thread   - cooperative priority, sample-rate loop, runs the DSP
 *   control_thread - preemptible, reads pots + encoder, updates fx params
 *   display_thread - lowest priority, refreshes the OLED status screen
 *
 * NOTE - this is a teaching-grade prototype, not a production audio
 * pipeline. The sample clock here is a k_timer, which is fine for an
 * 8 kHz mono delay effect but will jitter under system load. A
 * production version would trigger the ADC from a hardware timer and
 * move samples in/out with DMA instead of blocking adc_read_dt() calls
 * inside the loop - that upgrade path is good material to talk through
 * in an interview.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/display/cfb.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <string.h>

/* ---------- configuration ---------- */
#define SAMPLE_RATE_HZ    8000
#define SAMPLE_PERIOD_US  (1000000 / SAMPLE_RATE_HZ)

#define DELAY_MAX_MS      1000
#define DELAY_BUF_LEN     (SAMPLE_RATE_HZ * DELAY_MAX_MS / 1000)

#define DAC_RESOLUTION    12
#define DAC_MAX_VALUE     ((1 << DAC_RESOLUTION) - 1)

#define BYPASS_KEY_CODE   1
#define PRESET_KEY_CODE   2
#define ENCODER_SW_CODE   3

/* ---------- devicetree handles ---------- */
static const struct adc_dt_spec adc_channels[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0), /* audio in */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1), /* pot 1 (delay time) */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2), /* pot 2 (feedback) */
};

static const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac1));

static const struct dac_channel_cfg dac_cfg = {
	.channel_id = 1,
	.resolution = DAC_RESOLUTION,
	.buffered   = true,
};

static const struct device *qdec_dev = DEVICE_DT_GET(DT_NODELABEL(qdec));

/* ---------- shared effect state (audio thread writes/reads at 8 kHz,
 * control thread updates every 50 ms - atomics avoid a lock on the
 * audio thread's hot path) ---------- */
static struct {
	atomic_t delay_samples; /* 1 .. DELAY_BUF_LEN            */
	atomic_t feedback_pct;  /* 0 .. 90                        */
	atomic_t mix_pct;       /* 0 .. 100 (0 = dry, 100 = wet)   */
	atomic_t bypass;        /* 0 or 1                          */
} fx = {
	.delay_samples = ATOMIC_INIT(SAMPLE_RATE_HZ / 4), /* 250 ms default */
	.feedback_pct  = ATOMIC_INIT(35),
	.mix_pct       = ATOMIC_INIT(50),
	.bypass        = ATOMIC_INIT(0),
};

/* qdec reports an absolute, ever-accumulating position, so "reset to
 * 50%" is implemented as a subtractable offset rather than overwriting
 * fx.mix_pct directly - otherwise the next control-loop read of the
 * raw encoder position would immediately stomp the reset. */
static atomic_t encoder_offset = ATOMIC_INIT(0);
static atomic_t reset_mix_flag = ATOMIC_INIT(0);

/* ---------- audio thread ---------- */
static int16_t delay_buf[DELAY_BUF_LEN];
static size_t write_idx;

K_THREAD_STACK_DEFINE(audio_stack, 1024);
static struct k_thread audio_tid;

static int16_t adc_read_raw(const struct adc_dt_spec *spec)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer      = &raw,
		.buffer_size = sizeof(raw),
	};

	(void)adc_sequence_init_dt(spec, &seq);
	(void)adc_read_dt(spec, &seq);
	return raw;
}

static void audio_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct k_timer sample_timer;

	k_timer_init(&sample_timer, NULL, NULL);
	k_timer_start(&sample_timer, K_USEC(SAMPLE_PERIOD_US), K_USEC(SAMPLE_PERIOD_US));

	while (1) {
		k_timer_status_sync(&sample_timer);

		int16_t dry = adc_read_raw(&adc_channels[0]);

		if (atomic_get(&fx.bypass)) {
			dac_write_value(dac_dev, dac_cfg.channel_id, dry);
			continue;
		}

		int delay_n  = atomic_get(&fx.delay_samples);
		int feedback = atomic_get(&fx.feedback_pct);
		int mix      = atomic_get(&fx.mix_pct);

		size_t read_idx = (write_idx + DELAY_BUF_LEN - delay_n) % DELAY_BUF_LEN;
		int16_t delayed = delay_buf[read_idx];

		/* write dry signal + feedback of the delayed tap into the ring buffer */
		int32_t fed_back = dry + (delayed * feedback) / 100;

		if (fed_back > 2047) {
			fed_back = 2047;
		}
		if (fed_back < -2048) {
			fed_back = -2048;
		}
		delay_buf[write_idx] = (int16_t)fed_back;
		write_idx = (write_idx + 1) % DELAY_BUF_LEN;

		/* wet/dry mix, DAC expects unsigned 12-bit centered at 2048 */
		int32_t wet = (dry * (100 - mix) + delayed * mix) / 100;
		int32_t out = wet + 2048;

		if (out < 0) {
			out = 0;
		}
		if (out > DAC_MAX_VALUE) {
			out = DAC_MAX_VALUE;
		}

		dac_write_value(dac_dev, dac_cfg.channel_id, (uint32_t)out);
	}
}

/* ---------- control thread: pots, encoder, switches ---------- */
K_THREAD_STACK_DEFINE(control_stack, 1024);
static struct k_thread control_tid;

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || !evt->value) {
		return; /* only react to key-down, ignore key-up */
	}

	if (evt->code == BYPASS_KEY_CODE) {
		atomic_set(&fx.bypass, !atomic_get(&fx.bypass));
	} else if (evt->code == PRESET_KEY_CODE) {
		/* snap back to the default delay sound */
		atomic_set(&fx.delay_samples, SAMPLE_RATE_HZ / 4);
		atomic_set(&fx.feedback_pct, 35);
		atomic_set(&fx.mix_pct, 50);
	} else if (evt->code == ENCODER_SW_CODE) {
		/* defer the actual offset math to the control thread, which
		* has the current raw encoder reading close at hand */
		atomic_set(&reset_mix_flag, 1);
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static void control_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		/* pot 1 -> delay time, 10..1000 ms */
		int16_t pot1 = adc_read_raw(&adc_channels[1]);
		int delay_ms = 10 + (pot1 * (DELAY_MAX_MS - 10)) / 4095;

		atomic_set(&fx.delay_samples, delay_ms * SAMPLE_RATE_HZ / 1000);

		/* pot 2 -> feedback, 0..90 % */
		int16_t pot2 = adc_read_raw(&adc_channels[2]);

		atomic_set(&fx.feedback_pct, (pot2 * 90) / 4095);

		/* encoder -> wet/dry mix, 0..100 % */
		struct sensor_value rot;

		if (device_is_ready(qdec_dev) &&
		   sensor_sample_fetch(qdec_dev) == 0 &&
		   sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &rot) == 0) {
			int raw = rot.val1;

			if (atomic_get(&reset_mix_flag)) {
				atomic_set(&encoder_offset, raw - 50);
				atomic_set(&reset_mix_flag, 0);
			}

			int mix = (raw - atomic_get(&encoder_offset)) % 101;

			if (mix < 0) {
				mix += 101;
			}
			atomic_set(&fx.mix_pct, mix);
		}

		k_msleep(50);
	}
}

/* ---------- display thread ---------- */
K_THREAD_STACK_DEFINE(display_stack, 2048);
static struct k_thread display_tid;

static void display_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	bool have_display = device_is_ready(display_dev);

	if (!have_display) {
		printk("Display not ready - status will print over console instead\n");
	} else {
		cfb_framebuffer_init(display_dev);
		cfb_framebuffer_clear(display_dev, true);
	}

	char line[32];

	while (1) {
		int delay_ms = atomic_get(&fx.delay_samples) * 1000 / SAMPLE_RATE_HZ;
		int feedback = atomic_get(&fx.feedback_pct);
		int mix      = atomic_get(&fx.mix_pct);
		int bypass   = atomic_get(&fx.bypass);

		if (have_display) {
			cfb_framebuffer_clear(display_dev, false);

			snprintf(line, sizeof(line), "%s", bypass ? "BYPASS" : "DELAY FX");
			cfb_print(display_dev, line, 0, 0);

			snprintf(line, sizeof(line), "time %4d ms", delay_ms);
			cfb_print(display_dev, line, 0, 16);

			snprintf(line, sizeof(line), "fbck %3d %%", feedback);
			cfb_print(display_dev, line, 0, 32);

			snprintf(line, sizeof(line), "mix  %3d %%", mix);
			cfb_print(display_dev, line, 0, 48);

			cfb_framebuffer_finalize(display_dev);
		} else {
			printk("[%s] time=%dms fbck=%d%% mix=%d%%\n",
			      bypass ? "BYPASS" : "DELAY", delay_ms, feedback, mix);
		}

		k_msleep(200);
	}
}

/* ---------- startup ---------- */
int main(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			printk("ADC channel %d not ready\n", (int)i);
			return -1;
		}
		adc_channel_setup_dt(&adc_channels[i]);
	}

	if (!device_is_ready(dac_dev)) {
		printk("DAC not ready\n");
		return -1;
	}
	dac_channel_setup(dac_dev, &dac_cfg);

	if (!device_is_ready(qdec_dev)) {
		printk("Encoder (qdec) not ready - mix will stay at its default\n");
	}

	memset(delay_buf, 0, sizeof(delay_buf));

	/* cooperative priority: nothing preempts the audio thread mid-sample */
	k_thread_create(&audio_tid, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
			audio_thread_fn, NULL, NULL, NULL,
			K_PRIO_COOP(2), 0, K_NO_WAIT);

	k_thread_create(&control_tid, control_stack, K_THREAD_STACK_SIZEOF(control_stack),
			control_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

	k_thread_create(&display_tid, display_stack, K_THREAD_STACK_SIZEOF(display_stack),
			display_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);

	return 0;
}
