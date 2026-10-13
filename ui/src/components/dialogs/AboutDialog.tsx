/**
 * About dialog (useUi dialog type 'about').
 */
import { Button, Modal } from '@/components/ui';
import { AboutContent } from './about/AboutContent';
import { useStableCallback } from './common/hooks';

export function AboutDialog({ onClose }: { onClose: () => void }) {
  const close = useStableCallback(onClose);
  return (
    <Modal
      open
      title="About"
      onClose={close}
      width={560}
      height="min(640px, calc(100vh - 48px))"
      footer={
        <Button variant="primary" onClick={close}>
          Close
        </Button>
      }
    >
      <AboutContent />
    </Modal>
  );
}
