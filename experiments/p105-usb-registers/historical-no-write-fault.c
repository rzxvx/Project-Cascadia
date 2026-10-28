static int fault(struct cascadia_usb_registers *r, int ret)
{
	if (ret > 0)
		ret = -EIO;
	if (r->touched) {
		r->fault = true;
		r->fault_error = ret;
	}
	return ret;
}
