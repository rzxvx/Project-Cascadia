static int gate_ready(struct cascadia_controller *c, u64 epoch,
	unsigned int *value)
{
	int ret = read_checked(c, c->pmgr, epoch, EHCI_GATE, value);

	if (ret)
		return fault(c, ret);
	if (*value & RESET_BIT)
		return -EBUSY;
	/* Target and acknowledgement nibbles must both be on (0xf). */
	if ((*value & 0xff) != 0xff)
		return -EHOSTDOWN;
	return 0;
}
