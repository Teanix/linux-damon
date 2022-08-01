#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf.h>
#include <linux/version.h>
#include <bpf/bpf_helpers.h>

SEC("kprobe/test_input_4")
int bpf_prog1(struct pt_regs *ctx)
{
	unsigned long rc = -12;

	// bpf_override_return(ctx, rc);
	bpf_override_param(ctx, 10086,10087,10088,10089);
	return 0;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
