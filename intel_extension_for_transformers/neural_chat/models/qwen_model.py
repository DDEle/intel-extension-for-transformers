#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright (c) 2023 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from .base_model import BaseModel
import logging
from fastchat.conversation import get_conv_template, Conversation

logging.basicConfig(
    format="%(asctime)s - %(levelname)s - %(name)s - %(message)s",
    datefmt="%m/%d/%Y %H:%M:%S",
    level=logging.INFO,
)
logger = logging.getLogger(__name__)

def prepare_inputs_for_generation(
    input_ids, past_key_values=None, inputs_embeds=None, **kwargs
):
    if past_key_values:
        input_ids = input_ids[:, -1].unsqueeze(-1)

    attention_mask = kwargs.get("attention_mask", None)

    if inputs_embeds is not None and past_key_values is None:
        model_inputs = {"inputs_embeds": inputs_embeds}
    else:
        model_inputs = {"input_ids": input_ids}

    model_inputs.update(
        {
            "past_key_values": past_key_values,
            "use_cache": kwargs.get("use_cache"),
            "attention_mask": attention_mask,
        }
    )
    return model_inputs

class QwenModel(BaseModel):
    def match(self):
        """Check if the provided model_path matches the current model.

        Returns:
            bool: True if the model_path matches, False otherwise.
        """
        return "qwen" in self.model_name.lower()

    def get_default_conv_template(self) -> Conversation:
        """Get the default conversation template for the given model path.

        Args:
            model_path (str): Path to the model.

        Returns:
            Conversation: A default conversation template.
        """
        return get_conv_template("qwen-7b-chat")
